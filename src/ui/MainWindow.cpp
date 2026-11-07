#include "ui/MainWindow.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/ErrorMap.h"
#include "common/Lang.h"
#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/FileOps.h"
#include "core/HandleScanner.h"
#include "core/PathResolver.h"
#include "core/PolicyGuard.h"
#include "ipc/Json.h"
#include "ipc/PipeClient.h"
#include "ipc/Protocol.h"
#include "shell/ShellIntegration.h"
#include "ui/ElevationCheck.h"
#include "ui/FilePicker.h"
#include "ui/Theme.h"
#include "ui/UiIpc.h"

#pragma comment(lib, "comctl32.lib")

namespace ul {
namespace {

// ---------------------------------------------------------------- 控件与消息
constexpr int ID_ADD_FILE   = 1001;
constexpr int ID_ADD_DIR    = 1002;
constexpr int ID_DEEP_SCAN  = 1003;
constexpr int ID_FORCE      = 1004;
constexpr int ID_UNLOCK     = 1010;
constexpr int ID_DELETE     = 1011;
constexpr int ID_RENAME     = 1012;
constexpr int ID_MOVE       = 1013;
constexpr int ID_COPY       = 1014;
constexpr int ID_RESCAN     = 1015;
constexpr int ID_SETTINGS   = 1016;
constexpr int ID_MINIMIZE   = 1020;
constexpr int ID_MAXIMIZE   = 1021;
constexpr int ID_CLOSE      = 1022;

// 设置窗口内部控件
constexpr int ID_SET_LANG   = 2101;
constexpr int ID_SET_LINK   = 2102;
constexpr int ID_SET_SHELL  = 2103;
constexpr int ID_SET_APPEAR = 2104;

// 首次启动的右键菜单询问框
constexpr int ID_PROMPT_ADD   = 2201;
constexpr int ID_PROMPT_LATER = 2202;

// 版本号来自 CMake 的 UNLOCKER_VERSION_STR（target_compile_definitions），
// 只在命令行里单独编某个 .cpp 时才回落到占位串
#ifndef UNLOCKER_VERSION_STR
#define UNLOCKER_VERSION_STR L"0.0.0"
#endif
constexpr const wchar_t* kAppVersion = UNLOCKER_VERSION_STR;
constexpr const wchar_t* kAppName    = L"Unlocker";
constexpr const wchar_t* kRepoUrl    = L"https://github.com/LLLEIHH/Unlocker";

constexpr UINT WM_APP_SCAN_ITEM = WM_APP + 1;
constexpr UINT WM_APP_SCAN_DONE = WM_APP + 2;
constexpr UINT WM_APP_OP_DONE   = WM_APP + 3;
constexpr UINT WM_APP_STATUS    = WM_APP + 4;

// Fluent Icons / Segoe MDL2 Assets 码位（两套字体在这些码位上兼容）
constexpr const wchar_t* kGlyphAddFile  = L"\xE8E5";   // Add
constexpr const wchar_t* kGlyphAddDir   = L"\xE8B7";   // Folder
constexpr const wchar_t* kGlyphAdmin    = L"\xE7EF";   // Admin（盾牌）
constexpr const wchar_t* kGlyphUnlock   = L"\xE785";   // Unlock
constexpr const wchar_t* kGlyphDelete   = L"\xE74D";   // Delete
constexpr const wchar_t* kGlyphRename   = L"\xE70F";   // Edit
constexpr const wchar_t* kGlyphMove     = L"\xE8DE";   // MoveToFolder
constexpr const wchar_t* kGlyphCopy     = L"\xE8C8";   // Copy
constexpr const wchar_t* kGlyphRefresh  = L"\xE72C";   // Refresh
constexpr const wchar_t* kGlyphMin      = L"\xE921";   // ChromeMinimize
constexpr const wchar_t* kGlyphMax      = L"\xE922";   // ChromeMaximize
constexpr const wchar_t* kGlyphRestore  = L"\xE923";   // ChromeRestore
constexpr const wchar_t* kGlyphClose    = L"\xE8BB";   // ChromeClose
constexpr const wchar_t* kGlyphSettings = L"\xE713";   // Settings（齿轮）

constexpr int kFileColumns = 4;
constexpr int kProcColumns = 5;

// 子项路径缩写：只留相对目标的那一段，列表里才放得下
std::wstring RelativeToCanonical(const std::wstring& full, const std::wstring& base) {
    if (!full.empty() && !base.empty() && full.size() > base.size() + 1 &&
        _wcsnicmp(full.c_str(), base.c_str(), base.size()) == 0 && full[base.size()] == L'\\') {
        return full.substr(base.size() + 1);
    }
    return full;
}

struct UiOwner {
    DWORD        pid = 0;
    std::wstring name, desc, fullPath;
    std::wstring via;          // 命中的子项路径（目录目标的子树占用）
    int          handles = 1;
    bool         prot = false;
    bool         image = false;
};

struct UiItem {
    uint64_t              id = 0;
    std::wstring          path;
    std::wstring          canonical;
    OccupancyState        state = OccupancyState::Free;
    DWORD                 error = 0;
    unsigned long long    size = 0;
    bool                  dir = false;
    bool                  scanned = false;
    bool                  scanning = false;
    bool                  needElevation = false;
    int                   handles = 0;
    std::vector<UiOwner>  owners;
    std::vector<DWORD>    skipped;
    Verdict               verdict = Verdict::Allow;
    std::wstring          rule;
    std::wstring          reason;
};

struct ScanMsg {
    uint64_t id = 0;
    UiItem   item;
    bool     remove = false;
};

struct StatusMsg {
    std::wstring text;
};

struct OpDone {
    bool         ok = false;
    int          exitCode = 0;
    std::wstring summary;
    std::wstring detail;
};

struct App {
    HINSTANCE inst = nullptr;
    HWND      hwnd = nullptr;
    HWND      listFiles = nullptr;
    HWND      listProcs = nullptr;
    HWND      hdrFiles = nullptr;
    HWND      hdrProcs = nullptr;
    HWND      hint = nullptr;
    HWND      hintIcon = nullptr;
    HWND      btnAddFile = nullptr;
    HWND      btnAddDir = nullptr;
    HWND      btnDeep = nullptr;
    HWND      chkForce = nullptr;
    HWND      btnUnlock = nullptr;
    HWND      btnDelete = nullptr;
    HWND      btnRename = nullptr;
    HWND      btnMove = nullptr;
    HWND      btnCopy = nullptr;
    HWND      btnRescan = nullptr;
    HWND      btnSettings = nullptr;
    HWND      btnMin = nullptr;
    HWND      btnMax = nullptr;
    HWND      btnClose = nullptr;

    // 自绘需要的几何（Layout 里算好，WM_PAINT / 命中测试共用）
    RECT titleRc{};
    RECT bannerRc{};
    RECT fileCardRc{};
    RECT procCardRc{};
    RECT statusRc{};

    bool         bannerVisible = false;
    std::wstring bannerText;
    bool         statusAccent = false;
    int          titleCenterY = 0;   // 标题内容的垂直中心（Layout 按「窗口顶 → 工具按钮顶」算，保证上下留白相等）

    std::vector<UiItem> items;
    std::vector<int>    procKind;   // 进程列表每行的语义（0 占用 / 1 受保护 / 2 映射 / 3 子项 / 4 未知），与文案解耦
    uint64_t            nextId = 1;
    bool                elevated = false;
    bool                failClosed = false;
    bool                busy = false;
    bool                forceOn = false;
    std::wstring        statusText;

    std::unordered_map<HWND, bool> btnHot;

    PipeClient  worker;
    std::mutex  workerMutex;
    std::thread opThread;
    std::vector<std::thread> scanThreads;
};

App g;

// ---------------------------------------------------------------- 前置声明
void Layout(HWND hwnd);
void RefreshFileList();
void RefreshProcessList();
void UpdateButtons();
void SetStatus(const std::wstring& text);
void ApplyThemeToControls();
void DrawOwnerButton(const DRAWITEMSTRUCT* dis);

// ---------------------------------------------------------------- 小工具
std::wstring FileName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L'\\');
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring DirName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L'\\');
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

UiItem* FindItem(uint64_t id) {
    for (auto& it : g.items) if (it.id == id) return &it;
    return nullptr;
}

void SetStatus(const std::wstring& text) {
    g.statusText = text;
    g.statusAccent = g.busy || text.find(L"运行中") != std::wstring::npos ||
                     text.find(L"running") != std::wstring::npos;
    if (g.hwnd && !IsRectEmpty(&g.statusRc)) InvalidateRect(g.hwnd, &g.statusRc, FALSE);
}

void PostStatus(const std::wstring& text) {
    if (!g.hwnd) return;
    auto* msg = new StatusMsg{ text };
    if (!PostMessageW(g.hwnd, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
}

std::wstring VerdictText(Verdict v) {
    switch (v) {
        case Verdict::Deny:    return T(Str::StateBlocked);
        case Verdict::Confirm: return T(Str::StateConfirm);
        default:               return L"";
    }
}

// 状态列的语义色：只走 token，不出现颜色字面量
COLORREF StateSemanticColor(const UiItem& it) {
    if (it.verdict == Verdict::Deny) return theme::Color(theme::Token::Danger);
    if (!it.scanned) return theme::Color(theme::Token::TextTertiary);
    switch (it.state) {
        case OccupancyState::Free:           return theme::Color(theme::Token::Success);
        case OccupancyState::Locked:
        case OccupancyState::LockedByKernel: return theme::Color(theme::Token::Danger);
        case OccupancyState::ImageMapped:
        case OccupancyState::AccessDenied:   return theme::Color(theme::Token::Warning);
        case OccupancyState::Opened:         return theme::Color(theme::Token::TextSecondary);
        case OccupancyState::NotFound:
        case OccupancyState::Error:          return theme::Color(theme::Token::TextTertiary);
    }
    return theme::Color(theme::Token::TextPrimary);
}

std::wstring OwnersSummary(const UiItem& it) {
    if (it.owners.empty()) return L"—";
    std::wstring first = it.owners.front().name;
    if (first.empty()) first = L"PID " + std::to_wstring(it.owners.front().pid);
    if (it.owners.size() > 1) first += L" (+" + std::to_wstring(it.owners.size() - 1) + L")";
    return first;
}

std::wstring StateText(const UiItem& it) {
    if (it.verdict == Verdict::Deny) return T(Str::StateBlocked);
    if (!it.scanned) return it.scanning ? T(Str::StateScanning) : T(Str::StatePending);
    std::wstring t = OccupancyText(it.state);   // 内部已走 T()
    if (it.verdict == Verdict::Confirm) t = std::wstring(T(Str::StateConfirmPrefix)) + t;
    return t;
}

// ---------------------------------------------------------------- LNK 解析
std::wstring ResolveShortcut(const std::wstring& lnkPath, bool* ok) {
    if (ok) *ok = false;
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || !link) {
        return {};
    }
    IPersistFile* file = nullptr;
    std::wstring target;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file) {
        if (SUCCEEDED(file->Load(lnkPath.c_str(), STGM_READ)) && SUCCEEDED(link->Resolve(nullptr, SLR_NO_UI))) {
            wchar_t buf[MAX_PATH * 4]{};
            if (SUCCEEDED(link->GetPath(buf, ARRAYSIZE(buf), nullptr, 0)) && buf[0]) {
                target = buf;
                if (ok) *ok = true;
            }
        }
        file->Release();
    }
    link->Release();
    return target;
}

// ---------------------------------------------------------------- 列表刷新
void RefreshFileList() {
    if (!g.listFiles) return;
    SendMessageW(g.listFiles, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g.listFiles);

    for (size_t i = 0; i < g.items.size(); ++i) {
        const UiItem& it = g.items[i];
        LVITEMW lv{};
        lv.mask = LVIF_TEXT;
        lv.iItem = static_cast<int>(i);
        lv.pszText = const_cast<wchar_t*>(it.path.c_str());
        ListView_InsertItem(g.listFiles, &lv);

        // 注意：ListView_SetItemText 是宏，实参被拆成单独语句求值，
        // 临时 std::wstring 会在 SNDMSG 之前析构（悬空指针，列文字偶发空白）。必须先落成局部量。
        const std::wstring sizeText = it.dir ? T(Str::EntryIsDir) : FormatSize(it.size);
        const std::wstring ownerText = OwnersSummary(it);
        const std::wstring stateText = StateText(it);
        ListView_SetItemText(g.listFiles, static_cast<int>(i), 1,
                             const_cast<wchar_t*>(sizeText.c_str()));
        ListView_SetItemText(g.listFiles, static_cast<int>(i), 2,
                             const_cast<wchar_t*>(ownerText.c_str()));
        ListView_SetItemText(g.listFiles, static_cast<int>(i), 3,
                             const_cast<wchar_t*>(stateText.c_str()));
    }
    SendMessageW(g.listFiles, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.listFiles, nullptr, TRUE);

    if (g.hint) ShowWindow(g.hint, g.items.empty() ? SW_SHOW : SW_HIDE);
    if (g.hintIcon) ShowWindow(g.hintIcon, g.items.empty() ? SW_SHOW : SW_HIDE);
}

void RefreshProcessList() {
    if (!g.listProcs) return;
    SendMessageW(g.listProcs, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g.listProcs);
    g.procKind.clear();

    std::vector<const UiItem*> targets;
    const int sel = ListView_GetNextItem(g.listFiles, -1, LVNI_SELECTED);
    if (sel >= 0) {
        for (int i = sel; i >= 0; i = ListView_GetNextItem(g.listFiles, i, LVNI_SELECTED)) {
            if (static_cast<size_t>(i) < g.items.size()) targets.push_back(&g.items[i]);
        }
    } else {
        for (const auto& it : g.items) targets.push_back(&it);
    }

    std::vector<UiOwner> merged;
    for (const UiItem* it : targets) {
        for (const auto& o : it->owners) {
            bool found = false;
            for (auto& m : merged) {
                if (m.pid == o.pid) {
                    m.handles += o.handles;
                    if (m.via.empty()) m.via = o.via;
                    found = true;
                    break;
                }
            }
            if (!found) merged.push_back(o);
        }
    }

    int row = 0;
    for (const auto& o : merged) {
        LVITEMW lv{};
        lv.mask = LVIF_TEXT;
        lv.iItem = row;
        lv.pszText = const_cast<wchar_t*>(o.name.c_str());
        ListView_InsertItem(g.listProcs, &lv);
        const std::wstring pidText = std::to_wstring(o.pid);
        std::wstring descCol = o.desc.empty() ? L"—" : o.desc;
        if (!o.via.empty()) descCol += T(Str::OwnerChildPrefix) + o.via + T(Str::OwnerChildSuffix);
        const std::wstring handleText = L"×" + std::to_wstring(o.handles);

        // 语义与文案解耦：颜色按 kind 取，文字走 T()，切语言时颜色不会错乱
        int kind = 0;   // 占用
        if (o.prot) kind = 1;
        else if (o.image) kind = 2;
        else if (!o.via.empty()) kind = 3;
        const wchar_t* status = nullptr;
        switch (kind) {
            case 1: status = T(Str::ProcProtected); break;
            case 2: status = T(Str::ProcMapped); break;
            case 3: status = T(Str::ProcChildLock); break;
            default: status = T(Str::ProcInUse); break;
        }
        g.procKind.push_back(kind);

        ListView_SetItemText(g.listProcs, row, 1, const_cast<wchar_t*>(pidText.c_str()));
        ListView_SetItemText(g.listProcs, row, 2, const_cast<wchar_t*>(descCol.c_str()));
        ListView_SetItemText(g.listProcs, row, 3, const_cast<wchar_t*>(handleText.c_str()));
        ListView_SetItemText(g.listProcs, row, 4, const_cast<wchar_t*>(status));
        ++row;
    }

    // 结论可能被「看不到的进程」影响时，末尾汇总一行；不能让用户误以为「没人占用」
    int skipped = 0;
    for (const UiItem* it : targets) {
        if (it->state == OccupancyState::Locked || it->state == OccupancyState::LockedByKernel ||
            it->state == OccupancyState::AccessDenied) {
            skipped += static_cast<int>(it->skipped.size());
        }
    }
    if (skipped > 0) {
        std::wstring text = Tf(Str::ProcSkippedMany, skipped);
        LVITEMW lv{};
        lv.mask = LVIF_TEXT;
        lv.iItem = row;
        lv.pszText = const_cast<wchar_t*>(text.c_str());
        ListView_InsertItem(g.listProcs, &lv);
        g.procKind.push_back(4);
        ListView_SetItemText(g.listProcs, row, 4, const_cast<wchar_t*>(T(Str::ProcUnknown)));
    }
    SendMessageW(g.listProcs, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.listProcs, nullptr, TRUE);
}

void UpdateButtons() {
    const bool enable = !g.busy;
    for (HWND h : { g.btnAddFile, g.btnAddDir, g.btnUnlock, g.btnDelete, g.btnRename,
                    g.btnMove, g.btnCopy, g.btnRescan, g.btnDeep, g.chkForce }) {
        if (h) EnableWindow(h, enable);
    }
}

// ---------------------------------------------------------------- 扫描
void PostScanItem(uint64_t id, const UiItem& item, bool remove) {
    auto* msg = new ScanMsg{ id, item, remove };
    if (!PostMessageW(g.hwnd, WM_APP_SCAN_ITEM, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
}

UiItem MakeUiItem(const std::wstring& path, const PolicyResult& pol) {
    UiItem it;
    it.id = g.nextId++;
    it.path = path;
    it.canonical = pol.canonical;
    it.verdict = pol.verdict;
    it.rule = PolicyGuard::RuleName(pol.ruleId);
    it.reason = pol.reason;
    return it;
}

void StartLocalScan(const std::vector<std::pair<uint64_t, std::wstring>>& targets) {
    if (targets.empty()) return;
    const bool elevated = g.elevated;
    auto t = std::thread([targets, elevated] {
        for (const auto& [id, path] : targets) {
            FileReport rep = HandleScanner::Scan(path, ScanOptions{ elevated, true });

            UiItem it;
            it.id = id;
            it.path = path;
            it.canonical = rep.canonical;
            it.state = rep.state;
            it.error = rep.error;
            it.size = rep.size;
            it.dir = rep.directory;
            it.scanned = true;
            it.needElevation = rep.needElevation;
            it.handles = rep.totalFileHandles;
            it.skipped = rep.skippedPids;
            for (const auto& ow : rep.owners) {
                bool dup = false;
                for (const auto& uo : it.owners) if (uo.pid == ow.proc.pid) { dup = true; break; }
                if (dup) continue;
                UiOwner uo;
                uo.pid = ow.proc.pid;
                uo.name = ow.proc.name;
                uo.desc = ow.proc.description;
                uo.fullPath = ow.proc.fullPath;
                uo.handles = ow.proc.handleCount;
                uo.prot = ow.proc.protectedProc;
                uo.image = ow.isImageMapping;
                uo.via = RelativeToCanonical(ow.viaPath, rep.canonical);
                it.owners.push_back(std::move(uo));
            }
            PostScanItem(id, it, rep.state == OccupancyState::NotFound);
        }
        PostStatus(T(Str::StatusScanDone));
    });
    g.scanThreads.push_back(std::move(t));
}

// 深扫：一次请求 + 结果收集器（进度只用于状态栏）
void StartDeepScan(const std::vector<std::pair<uint64_t, std::wstring>>& targets) {
    if (targets.empty()) return;
    PostStatus(T(Str::StatusDeepRequest));

    auto t = std::thread([targets] {
        std::lock_guard<std::mutex> lk(g.workerMutex);
        std::wstring err;
        if (!g.worker.EnsureStarted(&err)) {
            PostStatus(err);
            auto* done = new OpDone{ false, kExitNeedElevation, err, L"" };
            PostMessageW(g.hwnd, WM_APP_OP_DONE, 2, reinterpret_cast<LPARAM>(done));
            return;
        }

        IpcRequest req;
        req.op = IpcOp::Scan;
        req.caller = "GUI";
        for (const auto& kv : targets) req.items.push_back(kv.second);

        json::Value result;
        bool got = false;
        const bool ok = g.worker.Request(req, [&](const json::Value& v) {
            if (v.GetString("evt") == "progress") {
                PostStatus(Tf(Str::StatusDeepProgress, static_cast<int>(v.GetNumber("cur", 0)), static_cast<int>(v.GetNumber("total", 0))));
                return;
            }
            result = v;
            got = true;
        }, &err, 300000);

        if (!ok || !got) {
            PostStatus(err.empty() ? T(Str::StatusDeepFailed) : err);
            auto* done = new OpDone{ false, kExitUnlockFailed, err, L"" };
            PostMessageW(g.hwnd, WM_APP_OP_DONE, 2, reinterpret_cast<LPARAM>(done));
            return;
        }

        const json::Value* results = result.Find("results");
        if (results && results->type == json::Value::Type::Array) {
            for (size_t i = 0; i < results->arr.size() && i < targets.size(); ++i) {
                const auto& o = results->arr[i];
                UiItem it;
                it.id = targets[i].first;
                it.path = Utf8ToUtf16(o.GetString("path"));
                it.canonical = Utf8ToUtf16(o.GetString("canonical"));
                it.size = static_cast<unsigned long long>(o.GetNumber("size", 0));
                it.dir = o.GetBool("dir");
                it.error = static_cast<DWORD>(o.GetNumber("error", 0));
                it.needElevation = o.GetBool("needElevation");
                it.handles = static_cast<int>(o.GetNumber("handles", 0));
                it.verdict = static_cast<Verdict>(o.GetNumber("verdict", 0));
                it.rule = Utf8ToUtf16(o.GetString("rule"));
                it.reason = Utf8ToUtf16(o.GetString("reason"));
                it.scanned = true;

                const std::string st = o.GetString("state");
                if (st == "free") it.state = OccupancyState::Free;
                else if (st == "opened") it.state = OccupancyState::Opened;
                else if (st == "locked") it.state = OccupancyState::Locked;
                else if (st == "imagemapped") it.state = OccupancyState::ImageMapped;
                else if (st == "accessdenied") it.state = OccupancyState::AccessDenied;
                else if (st == "lockedbykernel") it.state = OccupancyState::LockedByKernel;
                else if (st == "notfound") it.state = OccupancyState::NotFound;
                else it.state = OccupancyState::Error;

                const json::Value* owners = o.Find("owners");
                if (owners && owners->type == json::Value::Type::Array) {
                    for (const auto& ow : owners->arr) {
                        UiOwner uo;
                        uo.pid = static_cast<DWORD>(ow.GetNumber("pid", 0));
                        uo.name = Utf8ToUtf16(ow.GetString("name"));
                        uo.desc = Utf8ToUtf16(ow.GetString("desc"));
                        uo.fullPath = Utf8ToUtf16(ow.GetString("fullPath"));
                        uo.handles = static_cast<int>(ow.GetNumber("handles", 1));
                        uo.prot = ow.GetBool("protected");
                        uo.image = ow.GetBool("image");
                        uo.via = RelativeToCanonical(Utf8ToUtf16(ow.GetString("via")), it.canonical);
                        it.owners.push_back(std::move(uo));
                    }
                }
                PostScanItem(it.id, it, false);
            }
        }
        PostStatus(T(Str::StatusDeepDone));
        PostMessageW(g.hwnd, WM_APP_SCAN_DONE, 0, 0);
    });
    g.scanThreads.push_back(std::move(t));
}

// ---------------------------------------------------------------- 加入列表
void AddPaths(const std::vector<std::wstring>& raw) {
    if (raw.empty()) return;
    if (raw.size() > 500) {
        std::wstring msg = Tf(Str::DlgTooMany, raw.size());
        MessageBoxW(g.hwnd, msg.c_str(), L"Unlocker", MB_ICONINFORMATION | MB_OK);
        return;
    }

    std::vector<std::pair<uint64_t, std::wstring>> toScan;
    int rejected = 0;

    for (const auto& rawPath : raw) {
        std::wstring path = Trim(rawPath);
        if (path.empty()) continue;

        if (IsUncPath(path) && !IStartsWith(path, L"\\\\?\\")) {
            ++rejected;
            LogW(L"拒绝 UNC 路径：" + path);
            continue;
        }
        if (path.find(L"\\$Recycle.Bin\\") != std::wstring::npos) {
            ++rejected;
            LogW(L"拒绝回收站项目：" + path);
            continue;
        }

        if (IEndsWith(path, L".lnk")) {
            bool ok = false;
            const std::wstring target = ResolveShortcut(path, &ok);
            if (!ok) {
                ++rejected;
                continue;
            }
            path = target;   // 解析出的目标仍然要过黑名单（防快捷方式指向 ntoskrnl）
        }

        bool resolved = false;
        std::wstring canonical = PathResolver::CanonicalPath(path, &resolved);
        if (canonical.empty()) canonical = PathResolver::Normalize(path);
        if (canonical.empty()) continue;

        bool dup = false;
        for (const auto& it : g.items) {
            if (IEquals(it.canonical, canonical)) { dup = true; break; }
        }
        if (dup) continue;

        PolicyResult pol = PolicyGuard::Evaluate(path);
        UiItem it = MakeUiItem(path, pol);
        it.canonical = canonical;
        g.items.push_back(std::move(it));
        toScan.emplace_back(g.items.back().id, g.items.back().path);
    }

    if (toScan.empty()) {
        if (rejected)
            SetStatus(T(Str::StatusNothingToAdd));
        return;
    }

    for (auto& it : g.items) {
        for (const auto& kv : toScan) if (kv.first == it.id) it.scanning = true;
    }
    RefreshFileList();
    SetStatus(T(Str::StatusScanning));
    StartLocalScan(toScan);
}

// ---------------------------------------------------------------- 操作
std::vector<UiItem*> SelectedItems() {
    std::vector<UiItem*> out;
    int sel = ListView_GetNextItem(g.listFiles, -1, LVNI_SELECTED);
    if (sel < 0) {
        for (auto& it : g.items) out.push_back(&it);
        return out;
    }
    for (int i = sel; i >= 0; i = ListView_GetNextItem(g.listFiles, i, LVNI_SELECTED)) {
        if (static_cast<size_t>(i) < g.items.size()) out.push_back(&g.items[i]);
    }
    return out;
}

void RescanAll() {
    std::vector<std::pair<uint64_t, std::wstring>> targets;
    for (auto& it : g.items) {
        if (it.verdict == Verdict::Deny) continue;
        it.scanned = false;
        it.scanning = true;
        it.owners.clear();
        targets.emplace_back(it.id, it.path);
    }
    RefreshFileList();
    RefreshProcessList();
    if (targets.empty()) return;
    SetStatus(T(Str::StatusRescanning));
    StartLocalScan(targets);
}

void ApplyOpResult(const OpDone& done) {
    g.busy = false;
    UpdateButtons();
    SetStatus(done.ok ? T(Str::StatusOpDone)
                      : (done.summary.empty() ? T(Str::StatusOpFailed) : done.summary));

    // 用户点了 UAC 的「否」是正常路径：一行提示 + 重试入口，不当异常弹错误框
    const bool cancelled = done.summary.find(T(Str::ErrElevationCancelled)) != std::wstring::npos ||
                           done.summary.find(L"已取消提权") != std::wstring::npos;
    if (cancelled) {
        MessageBoxW(g.hwnd,
                    T(Str::DlgElevationCancelled),
                    L"Unlocker", MB_ICONINFORMATION | MB_OK);
        return;
    }

    std::wstring text = done.summary;
    if (!done.detail.empty()) {
        text += L"\n\n";
        text += done.detail;
    }
    MessageBoxW(g.hwnd, text.c_str(), done.ok ? L"Unlocker" : L"Unlocker · Not completed",
                (done.ok ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_OK);

    RescanAll();
}

void StartOp(IpcOp op, const std::wstring& arg) {
    if (g.busy) return;

    auto targets = SelectedItems();
    std::vector<std::wstring> paths;
    int denied = 0;
    for (auto* it : targets) {
        if (!it) continue;
        if (it->verdict == Verdict::Deny) { ++denied; continue; }
        if (it->state == OccupancyState::NotFound) continue;
        paths.push_back(it->path);
    }
    if (paths.empty()) {
        MessageBoxW(g.hwnd,
                    denied ? T(Str::DlgNoTargetsDenied)
                           : T(Str::DlgNoTargets),
                    L"Unlocker", MB_ICONINFORMATION | MB_OK);
        return;
    }

    g.busy = true;
    UpdateButtons();
    const bool force = g.forceOn;
    SetStatus(T(Str::StatusOpRequest));

    if (g.opThread.joinable()) g.opThread.join();
    g.opThread = std::thread([op, arg, paths, force] {
        std::lock_guard<std::mutex> lk(g.workerMutex);
        std::wstring err;
        if (!g.worker.EnsureStarted(&err)) {
            auto* done = new OpDone{ false, kExitNeedElevation, err, L"" };
            PostMessageW(g.hwnd, WM_APP_OP_DONE, 1, reinterpret_cast<LPARAM>(done));
            return;
        }

        IpcRequest req;
        req.op = op;
        req.items = paths;
        req.arg = arg;
        req.force = force;
        req.caller = "GUI";

        json::Value result;
        bool got = false;
        const bool ok = g.worker.Request(req, [](const json::Value& v) {
            if (v.GetString("evt") == "progress") {
                PostStatus(Tf(Str::StatusOpProgress, static_cast<int>(v.GetNumber("cur", 0)), static_cast<int>(v.GetNumber("total", 0))));
                return;
            }
            PostStatus(T(Str::StatusOpRunning));
        }, &err, 300000);

        if (!ok || !got) {
            (void)result;
            auto* done = new OpDone{ false, kExitUnlockFailed, err, L"" };
            PostMessageW(g.hwnd, WM_APP_OP_DONE, 1, reinterpret_cast<LPARAM>(done));
            return;
        }

        auto* done = new OpDone{};
        done->ok = result.GetBool("ok", false);
        done->exitCode = static_cast<int>(result.GetNumber("code", 0));

        std::wstring summary = Tf(Str::OpSummary, static_cast<int>(result.GetNumber("okCount", 0)), static_cast<int>(result.GetNumber("failCount", 0)), static_cast<int>(result.GetNumber("skipCount", 0)));
        if (result.GetBool("reboot", false)) {
            summary += T(Str::OpRebootNote);
        }
        done->summary = summary;

        std::wstring detail;
        const json::Value* results = result.Find("results");
        if (results && results->type == json::Value::Type::Array) {
            for (const auto& it : results->arr) {
                const bool itemOk = it.GetBool("ok");
                const bool skipped = it.GetBool("skipped");
                if (itemOk && !skipped) continue;
                if (detail.size() > 4000) break;
                detail += (skipped ? T(Str::OpDetailSkipped) : T(Str::OpDetailFailed)) +
                          Utf8ToUtf16(it.GetString("path")) + L"\n";
                const std::wstring rule = Utf8ToUtf16(it.GetString("rule"));
                if (!rule.empty()) detail += T(Str::OpDetailRule) + rule + L"\n";
                const std::wstring msg = Utf8ToUtf16(it.GetString("msg"));
                if (!msg.empty()) detail += L"    " + msg + L"\n";
            }
        }
        done->detail = detail;
        PostMessageW(g.hwnd, WM_APP_OP_DONE, 1, reinterpret_cast<LPARAM>(done));
    });
}

// ---------------------------------------------------------------- 尺寸计算
int ButtonWidth(HDC dc, HWND h, const wchar_t* glyph) {
    wchar_t buf[128]{};
    if (h) GetWindowTextW(h, buf, ARRAYSIZE(buf));
    int w = theme::MeasureText(dc, buf, theme::FontBody());
    if (glyph && *glyph) w += theme::MeasureText(dc, glyph, theme::FontIcon()) + theme::Scale(8);
    w += theme::ControlPadX() * 2;   // 左右各 15px（150%）：固定，不随文本长度变化
    // 下限只防「文本为空」这类异常，不能把短标签撑宽而破坏固定内边距
    return std::max(w, theme::Scale(40));
}

int CheckWidth(HDC dc, HWND h) {
    wchar_t buf[128]{};
    if (h) GetWindowTextW(h, buf, ARRAYSIZE(buf));
    return theme::ControlPadX() + theme::Scale(16) + theme::Scale(8) +
           theme::MeasureText(dc, buf, theme::FontBody()) + theme::ControlPadX();
}

// 纯图标按钮（齿轮）：宽度只由图标决定，左右内边距同样固定 15px（150%）。
// 图标字体单字的前进宽度里含很宽的左右边距（实测各 9px @150%），
// 按前进宽度算会让肉眼看到的留白变成 24px，所以这里按「墨迹宽度」算。
int IconButtonWidth(HDC dc, const wchar_t* glyph) {
    if (!glyph || !*glyph) return theme::ControlPadX() * 2;
    int ink = theme::MeasureGlyphInk(dc, glyph[0], theme::FontIcon());
    if (ink <= 0) ink = theme::MeasureText(dc, glyph, theme::FontIcon());
    return theme::ControlPadX() * 2 + ink;
}

void SetCardRegion(HWND h, int w, int hgt, int radius) {
    if (!h || w <= 0 || hgt <= 0) return;
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, hgt + 1, radius * 2, radius * 2);
    if (rgn) SetWindowRgn(h, rgn, TRUE);   // 成功后 region 归系统所有，不要再 DeleteObject
}

// 列宽自适应：把剩余宽度补给「弹性列」（路径/描述），窄窗口下不出横向滚动条
void FitListWidth(HWND list, int flexCol, const int* baseWidth, int count) {
    if (!list) return;
    RECT rc{};
    GetClientRect(list, &rc);
    if (rc.right <= rc.left) return;

    int total = 0;
    int cols[8]{};
    for (int i = 0; i < count && i < 8; ++i) {
        cols[i] = theme::Scale(baseWidth[i]);
        total += cols[i];
    }
    int flex = cols[flexCol] + (rc.right - total);
    if (flex < theme::Scale(150)) flex = theme::Scale(150);
    SendMessageW(list, LVM_SETCOLUMNWIDTH, static_cast<WPARAM>(flexCol), MAKELPARAM(flex, 0));
}

// ---------------------------------------------------------------- 布局
void Layout(HWND hwnd) {
    if (!hwnd || !g.btnAddFile) return;
    RECT client{};
    GetClientRect(hwnd, &client);
    const int W = client.right, H = client.bottom;

    const int pad     = theme::Scale(12);
    const int gap     = theme::Scale(6);
    const int titleH  = theme::Scale(32);
    const int btnH    = theme::Scale(30);
    const int capW    = theme::Scale(46);
    const int cardGap = theme::Scale(8);
    const int cardR   = theme::Scale(8);

    g.titleRc = { 0, 0, W, titleH };

    // 标题栏窗口按钮：右上角三格，每格 46×32
    int bx = W - capW;
    MoveWindow(g.btnClose, bx, 0, capW, titleH, TRUE); bx -= capW;
    MoveWindow(g.btnMax, bx, 0, capW, titleH, TRUE); bx -= capW;
    MoveWindow(g.btnMin, bx, 0, capW, titleH, TRUE);

    int y = titleH + theme::Scale(4);

    if (g.bannerVisible) {
        g.bannerRc = { pad, y, W - pad, y + theme::Scale(34) };
        y = g.bannerRc.bottom + theme::Scale(8);
    } else {
        SetRectEmpty(&g.bannerRc);
    }

    // 工具栏：按钮高 30，工具栏带高 36，左侧内边距 12
    const int toolbarH = theme::Scale(36);
    const int toolY = y + (toolbarH - btnH) / 2;
    const int toolBottom = y + toolbarH;

    // 标题内容在「窗口顶 → 工具按钮顶」这段可见区域里居中：
    // 只按标题带居中会让上方留白只有下方的一半，看着顶到天花板
    const int box = theme::Scale(22);
    g.titleCenterY = std::min(toolY / 2, titleH - box / 2);

    HDC dc = GetDC(hwnd);
    int x = pad;
    struct ToolBtn { HWND h; const wchar_t* glyph; };
    const ToolBtn tools[] = { { g.btnAddFile, kGlyphAddFile },
                              { g.btnAddDir, kGlyphAddDir },
                              { g.btnDeep, kGlyphAdmin } };
    for (const auto& t : tools) {
        const int w = ButtonWidth(dc, t.h, t.glyph);
        MoveWindow(t.h, x, toolY, w, btnH, TRUE);
        x += w + gap;
    }
    const int chkW = CheckWidth(dc, g.chkForce);
    // 复选框自带 15px 左内边距，不再额外加偏移，保持与其它按钮一致的节奏
    MoveWindow(g.chkForce, x, toolY, chkW, btnH, TRUE);

    // 设置（齿轮）按钮固定在工具栏最右，宽度 = 15 + 图标墨迹 + 15
    const int gearW = IconButtonWidth(dc, kGlyphSettings);
    MoveWindow(g.btnSettings, W - pad - gearW, toolY, gearW, btnH, TRUE);

    // 底部操作条：按钮右对齐，状态文字占左侧剩余宽度；太窄时状态文字换到上一行
    struct ActBtn { HWND h; const wchar_t* glyph; };
    const ActBtn acts[] = { { g.btnUnlock, kGlyphUnlock }, { g.btnDelete, kGlyphDelete },
                            { g.btnRename, kGlyphRename }, { g.btnMove, kGlyphMove },
                            { g.btnCopy, kGlyphCopy },     { g.btnRescan, kGlyphRefresh } };
    int widths[6]{};
    int total = gap * 5;
    for (int i = 0; i < 6; ++i) {
        widths[i] = ButtonWidth(dc, acts[i].h, acts[i].glyph);
        total += widths[i];
    }
    ReleaseDC(hwnd, dc);

    const bool stackBar = (total + theme::Scale(230) + pad * 2 > W);
    const int barH = stackBar ? theme::Scale(76) : theme::Scale(44);
    const int barY = H - barH;
    int ax = W - pad - total;
    const int actY = stackBar ? (H - theme::Scale(44) + (theme::Scale(44) - btnH) / 2)
                              : (barY + (barH - btnH) / 2);
    for (int i = 0; i < 6; ++i) {
        MoveWindow(acts[i].h, ax, actY, widths[i], btnH, TRUE);
        ax += widths[i] + gap;
    }
    if (stackBar) {
        g.statusRc = { pad, barY + theme::Scale(6), W - pad, barY + theme::Scale(6) + theme::Scale(22) };
    } else {
        g.statusRc = { pad, barY, std::max(pad + theme::Scale(40), W - pad - total - theme::Scale(12)),
                       barY + barH };
    }

    // 卡片区
    const int top = toolBottom + theme::Scale(8);
    int avail = barY - theme::Scale(8) - top;
    if (avail < theme::Scale(180)) avail = theme::Scale(180);
    int fileH = avail * 55 / 100;
    int procH = avail - fileH - cardGap;
    if (procH < theme::Scale(96)) {
        procH = std::min(theme::Scale(96), avail / 2);
        fileH = avail - procH - cardGap;
    }

    g.fileCardRc = { pad, top, W - pad, top + fileH };
    g.procCardRc = { pad, g.fileCardRc.bottom + cardGap, W - pad,
                     g.fileCardRc.bottom + cardGap + procH };

    RECT files = g.fileCardRc;
    InflateRect(&files, -1, -1);
    RECT procs = g.procCardRc;
    InflateRect(&procs, -1, -1);

    const int fw = files.right - files.left, fh = files.bottom - files.top;
    const int pw = procs.right - procs.left, ph = procs.bottom - procs.top;
    MoveWindow(g.listFiles, files.left, files.top, fw, fh, TRUE);
    MoveWindow(g.listProcs, procs.left, procs.top, pw, ph, TRUE);
    SetCardRegion(g.listFiles, fw, fh, cardR - 1);
    SetCardRegion(g.listProcs, pw, ph, cardR - 1);

    static const int kFileBase[kFileColumns] = { 430, 90, 190, 130 };
    static const int kProcBase[kProcColumns] = { 220, 70, 260, 60, 90 };
    FitListWidth(g.listFiles, 0, kFileBase, kFileColumns);
    FitListWidth(g.listProcs, 2, kProcBase, kProcColumns);

    // 空态提示：避开表头（否则盖住列名），图示 + 文字整体垂直居中
    int headerH = 0;
    if (g.hdrFiles && IsWindow(g.hdrFiles)) {
        RECT hr{};
        GetWindowRect(g.hdrFiles, &hr);
        headerH = hr.bottom - hr.top;
    }
    const int hintTop = files.top + headerH;
    const int iconH = theme::Scale(46);
    const int textH = theme::Scale(44);
    const int blockH = iconH + theme::Scale(4) + textH;
    const int blockTop = hintTop + std::max(0, static_cast<int>(files.bottom - hintTop - blockH) / 2);
    MoveWindow(g.hintIcon, files.left + theme::Scale(8), blockTop, fw - theme::Scale(16), iconH, TRUE);
    MoveWindow(g.hint, files.left + theme::Scale(8), blockTop + iconH + theme::Scale(4),
               fw - theme::Scale(16), textH, TRUE);
}

// ---------------------------------------------------------------- 自绘：框架
void PaintTitleBar(HDC dc) {
    if (!g.hwnd || IsRectEmpty(&g.titleRc)) return;
    const int h = theme::Scale(32);
    const int pad = theme::Scale(14);
    const int box = theme::Scale(22);
    const int cy = g.titleCenterY > 0 ? g.titleCenterY : h / 2;
    const bool active = GetForegroundWindow() == g.hwnd;

    RECT logo{ pad, cy - box / 2, pad + box, cy + box / 2 };
    theme::FillRound(dc, logo, theme::Scale(6), theme::Color(theme::Token::Accent));
    theme::DrawTextLine(dc, L"\xE785", logo, theme::FontIconSmall(),
                        theme::Color(theme::Token::AccentText),
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // 文本矩形整体跟着 cy 平移，保持 DT_VCENTER 的作用点与图标中心一致
    const int textTop = cy - h / 2;
    const int textBottom = cy + h / 2;

    const int nameX = logo.right + theme::Scale(10);
    const std::wstring name = L"Unlocker";
    const int nameW = theme::MeasureText(dc, name, theme::FontStrong());
    RECT tr{ nameX, textTop, nameX + nameW + theme::Scale(2), textBottom };
    theme::DrawTextLine(dc, name, tr, theme::FontStrong(),
                        theme::Color(active ? theme::Token::TextPrimary : theme::Token::TextSecondary),
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT sr{ tr.right + theme::Scale(8), textTop, g.titleRc.right - theme::Scale(160), textBottom };
    theme::DrawTextLine(dc, T(Str::AppSubtitle), sr, theme::FontCaption(),
                        theme::Color(theme::Token::TextTertiary),
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void PaintBanner(HDC dc) {
    if (!g.bannerVisible || IsRectEmpty(&g.bannerRc)) return;
    theme::FillRound(dc, g.bannerRc, theme::Scale(6), theme::Color(theme::Token::WarningBg));

    RECT bar{ g.bannerRc.left + theme::Scale(1), g.bannerRc.top + theme::Scale(9),
              g.bannerRc.left + theme::Scale(4), g.bannerRc.bottom - theme::Scale(9) };
    theme::FillRound(dc, bar, theme::Scale(2), theme::Color(theme::Token::Warning));

    RECT tr = g.bannerRc;
    tr.left = bar.right + theme::Scale(10);
    tr.right -= theme::Scale(12);
    theme::DrawTextLine(dc, g.bannerText, tr, theme::FontBody(),
                        theme::Color(theme::Token::TextPrimary),
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void PaintCards(HDC dc) {
    if (!IsRectEmpty(&g.fileCardRc)) theme::DrawCard(dc, g.fileCardRc, theme::Scale(8));
    if (!IsRectEmpty(&g.procCardRc)) theme::DrawCard(dc, g.procCardRc, theme::Scale(8));
}

void PaintStatus(HDC dc) {
    if (g.statusText.empty() || IsRectEmpty(&g.statusRc)) return;
    const int cy = (g.statusRc.top + g.statusRc.bottom) / 2;
    const int r = theme::Scale(3);
    RECT dot{ g.statusRc.left, cy - r, g.statusRc.left + r * 2, cy + r };
    theme::FillRound(dc, dot, r,
                     theme::Color(g.statusAccent ? theme::Token::Accent : theme::Token::TextTertiary));

    RECT tr{ dot.right + theme::Scale(8), g.statusRc.top, g.statusRc.right, g.statusRc.bottom };
    theme::DrawTextLine(dc, g.statusText, tr, theme::FontCaption(),
                        theme::Color(theme::Token::TextSecondary),
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------- 自绘：行
void DrawCells(HDC dc, HWND list, int item, const RECT& row, const COLORREF* colors, int count) {
    const int pad = theme::Scale(10);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, row.left, row.top, row.right, row.bottom);

    RECT cell = row;
    for (int col = 0; col < count && cell.left < row.right; ++col) {
        const int w = static_cast<int>(
            SendMessageW(list, LVM_GETCOLUMNWIDTH, static_cast<WPARAM>(col), 0));
        cell.right = cell.left + w;

        wchar_t buf[512]{};
        ListView_GetItemText(list, item, col, buf, ARRAYSIZE(buf));
        if (buf[0]) {
            RECT tr = cell;
            tr.left += pad + (col == 0 ? theme::Scale(6) : 0);
            tr.right -= theme::Scale(4);
            theme::DrawTextLine(dc, buf, tr, theme::FontBody(), colors[col],
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        cell.left = cell.right;
    }
    RestoreDC(dc, saved);
}

void DrawFocusRing(const DRAWITEMSTRUCT* dis) {
    if ((dis->itemState & ODS_FOCUS) == 0 || theme::HighContrast()) return;
    RECT fr = dis->rcItem;
    InflateRect(&fr, -theme::Scale(1), -theme::Scale(1));
    theme::StrokeRound(dis->hDC, fr, 0, theme::Color(theme::Token::Accent), 1);
}

void DrawFileRow(const DRAWITEMSTRUCT* dis) {
    const int idx = static_cast<int>(dis->itemID);
    if (idx < 0 || static_cast<size_t>(idx) >= g.items.size()) {
        theme::FillSolid(dis->hDC, dis->rcItem, theme::Color(theme::Token::CardBg));
        return;
    }
    const UiItem& it = g.items[static_cast<size_t>(idx)];
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;

    COLORREF bg = theme::Color(theme::Token::CardBg);
    if (it.verdict == Verdict::Deny) bg = theme::Color(theme::Token::DangerBg);
    if (selected) bg = theme::Color(theme::Token::SelectedBg);
    theme::FillSolid(dis->hDC, dis->rcItem, bg);

    if (selected) {
        RECT bar = dis->rcItem;
        bar.left += theme::Scale(4);
        bar.right = bar.left + theme::Scale(3);
        InflateRect(&bar, 0, -theme::Scale(7));
        theme::FillRound(dis->hDC, bar, theme::Scale(2), theme::Color(theme::Token::Accent));
    }

    const COLORREF colors[kFileColumns] = {
        theme::Color(theme::Token::TextPrimary),
        theme::Color(theme::Token::TextSecondary),
        theme::Color(theme::Token::TextPrimary),
        StateSemanticColor(it),
    };
    DrawCells(dis->hDC, dis->hwndItem, idx, dis->rcItem, colors, kFileColumns);
    DrawFocusRing(dis);
}

COLORREF ProcStateColor(int item) {
    const int kind = (item >= 0 && static_cast<size_t>(item) < g.procKind.size()) ? g.procKind[item] : 0;
    switch (kind) {
        case 1:          return theme::Color(theme::Token::Danger);      // 受保护
        case 2:          return theme::Color(theme::Token::Warning);     // 映射
        case 3:          return theme::Color(theme::Token::Warning);     // 子项占用
        case 4:          return theme::Color(theme::Token::TextTertiary); // 未知
        default:         return theme::Color(theme::Token::TextSecondary);
    }
}

void DrawProcRow(const DRAWITEMSTRUCT* dis) {
    const int idx = static_cast<int>(dis->itemID);
    if (idx < 0) return;
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;

    COLORREF bg = theme::Color(theme::Token::CardBg);
    if (selected) bg = theme::Color(theme::Token::SelectedBg);
    theme::FillSolid(dis->hDC, dis->rcItem, bg);

    if (selected) {
        RECT bar = dis->rcItem;
        bar.left += theme::Scale(4);
        bar.right = bar.left + theme::Scale(3);
        InflateRect(&bar, 0, -theme::Scale(7));
        theme::FillRound(dis->hDC, bar, theme::Scale(2), theme::Color(theme::Token::Accent));
    }

    const COLORREF colors[kProcColumns] = {
        theme::Color(theme::Token::TextPrimary),
        theme::Color(theme::Token::TextSecondary),
        theme::Color(theme::Token::TextSecondary),
        theme::Color(theme::Token::TextSecondary),
        ProcStateColor(idx),
    };
    DrawCells(dis->hDC, dis->hwndItem, idx, dis->rcItem, colors, kProcColumns);
    DrawFocusRing(dis);
}

// owner-draw 的 ListView 会在“没有行”的空白区画出列分隔线，POSTPAINT 里整块刷回卡片色
void FillListTail(HWND list, HDC dc) {
    if (!list || !dc) return;
    RECT client{};
    GetClientRect(list, &client);

    int tailTop = client.top;
    const int count = ListView_GetItemCount(list);
    if (count > 0) {
        RECT ir{};
        if (ListView_GetItemRect(list, count - 1, &ir, LVIR_BOUNDS)) tailTop = ir.bottom;
    } else {
        HWND header = reinterpret_cast<HWND>(SendMessageW(list, LVM_GETHEADER, 0, 0));
        if (header) {
            RECT hr{};
            GetWindowRect(header, &hr);
            tailTop = hr.bottom - hr.top;
        }
    }
    if (tailTop >= client.bottom) return;

    RECT tail{ client.left, tailTop, client.right, client.bottom };
    theme::FillSolid(dc, tail, theme::Color(theme::Token::CardBg));
}

// 表头整条自绘：系统在经典/主题两种模式下都会补画分隔线与 hover 高亮，
// 干脆在 POSTPAINT 里把整条重画一遍，保证只剩「卡片底色 + 列名 + 一条分割线」
void DrawHeaderAll(HWND header, HDC dc) {
    if (!header || !dc) return;
    RECT rc{};
    GetClientRect(header, &rc);
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    theme::FillSolid(dc, rc, theme::Color(theme::Token::CardBg));

    const int count = Header_GetItemCount(header);
    for (int i = 0; i < count; ++i) {
        RECT ir{};
        if (!Header_GetItemRect(header, i, &ir)) continue;

        wchar_t buf[128]{};
        HDITEMW item{};
        item.mask = HDI_TEXT;
        item.pszText = buf;
        item.cchTextMax = ARRAYSIZE(buf);
        if (!Header_GetItem(header, i, &item)) continue;

        RECT tr = ir;
        tr.left += theme::Scale(10) + (i == 0 ? theme::Scale(6) : 0);
        tr.right -= theme::Scale(4);
        theme::DrawTextLine(dc, buf, tr, theme::FontCaption(),
                            theme::Color(theme::Token::TextSecondary),
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    RECT line{ rc.left, rc.bottom - theme::Scale(1), rc.right, rc.bottom };
    theme::FillSolid(dc, line, theme::Color(theme::Token::Divider));
}

// 表头的父窗口是 ListView，它不会把 NM_CUSTOMDRAW 转发给我们，
// 所以表头只能靠子类化直接接管 WM_PAINT（这是唯一稳定生效的自绘入口）
LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR idSubclass, DWORD_PTR) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            DrawHeaderAll(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;   // 整条由 WM_PAINT 覆盖
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, HeaderSubclassProc, idSubclass);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------- 自绘：按钮
LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR idSubclass, DWORD_PTR) {
    switch (msg) {
        case WM_KEYDOWN:
            // 自绘按钮不实现对话框的默认按钮语义，这里把回车/ESC 转给父窗口，
            // 首次询问框就能「回车=添加、ESC=暂不」，设置窗口也能用 ESC 关闭
            if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
                PostMessageW(GetParent(hwnd), WM_COMMAND,
                             MAKEWPARAM(wParam == VK_RETURN ? IDOK : IDCANCEL, 0),
                             reinterpret_cast<LPARAM>(hwnd));
                return 0;
            }
            break;
        case WM_SETCURSOR:
            // 链接按钮：手型光标（其余按钮保持箭头）
            if (LOWORD(lParam) == HTCLIENT && GetDlgCtrlID(hwnd) == ID_SET_LINK) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE:
            if (!g.btnHot[hwnd]) {
                g.btnHot[hwnd] = true;
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (g.btnHot[hwnd]) {
                g.btnHot[hwnd] = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_ERASEBKGND:
            return 1;   // 整块由 WM_DRAWITEM 覆盖，先刷一遍反而闪
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_NCDESTROY:
            g.btnHot.erase(hwnd);
            RemoveWindowSubclass(hwnd, ButtonSubclassProc, idSubclass);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void DrawOwnerButton(const DRAWITEMSTRUCT* dis) {
    const int id = static_cast<int>(dis->CtlID);
    theme::ButtonPaint p;
    p.rc = dis->rcItem;
    p.disabled = (dis->itemState & ODS_DISABLED) != 0;
    p.pressed = (dis->itemState & ODS_SELECTED) != 0;
    p.focused = (dis->itemState & ODS_FOCUS) != 0 && !p.disabled;
    auto hot = g.btnHot.find(dis->hwndItem);
    p.hot = hot != g.btnHot.end() && hot->second;

    switch (id) {
        case ID_ADD_FILE: p.glyph = kGlyphAddFile; break;
        case ID_ADD_DIR:  p.glyph = kGlyphAddDir; break;
        case ID_DEEP_SCAN: p.glyph = kGlyphAdmin; break;
        case ID_FORCE:
            p.checkbox = true;
            p.checked = g.forceOn;
            break;
        case ID_UNLOCK: p.glyph = kGlyphUnlock; p.primary = true; break;
        case ID_DELETE: p.glyph = kGlyphDelete; break;
        case ID_RENAME: p.glyph = kGlyphRename; break;
        case ID_MOVE:   p.glyph = kGlyphMove; break;
        case ID_COPY:   p.glyph = kGlyphCopy; break;
        case ID_RESCAN: p.glyph = kGlyphRefresh; break;
        case ID_SETTINGS: p.glyph = kGlyphSettings; break;
        case ID_MINIMIZE:
            p.caption = true;
            p.glyph = kGlyphMin;
            break;
        case ID_MAXIMIZE:
            p.caption = true;
            p.glyph = IsZoomed(g.hwnd) ? kGlyphRestore : kGlyphMax;
            break;
        case ID_CLOSE:
            p.caption = true;
            p.glyph = kGlyphClose;
            p.closeStyle = true;
            break;
        default:
            break;
    }

    if (p.caption) {
        // 标题栏按钮的窗口文字只留给讲述人（UIA Name），画面只用 glyph
        p.text.clear();
    } else if (id == ID_SETTINGS) {
        // 窗口文字只给讲述人读（UIA Name），画面只画齿轮
        p.text.clear();
    } else {
        wchar_t label[128]{};
        GetWindowTextW(dis->hwndItem, label, ARRAYSIZE(label));
        p.text = label;
    }

    theme::PaintButton(dis->hDC, p);
}

// ---------------------------------------------------------------- 主题应用
void ApplyThemeToControls() {
    for (HWND lv : { g.listFiles, g.listProcs }) {
        if (!lv) continue;
        ListView_SetBkColor(lv, theme::Color(theme::Token::CardBg));
        ListView_SetTextBkColor(lv, theme::Color(theme::Token::CardBg));
        ListView_SetTextColor(lv, theme::Color(theme::Token::TextPrimary));
        SendMessageW(lv, WM_SETFONT, reinterpret_cast<WPARAM>(theme::FontBody()), TRUE);
        theme::ApplyChildTheme(lv);
        HWND header = reinterpret_cast<HWND>(SendMessageW(lv, LVM_GETHEADER, 0, 0));
        if (header) {
            SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(theme::FontBody()), TRUE);
            // 表头整块自绘：关掉视觉样式 + 接管 WM_PAINT，避免系统补画分隔线与 hover 高亮
            theme::ClearChildTheme(header);
            SetWindowSubclass(header, HeaderSubclassProc, 1, 0);
        }
    }
    if (g.hint) {
        SendMessageW(g.hint, WM_SETFONT, reinterpret_cast<WPARAM>(theme::FontBody()), TRUE);
        InvalidateRect(g.hint, nullptr, TRUE);
    }
    if (g.hintIcon) {
        SendMessageW(g.hintIcon, WM_SETFONT, reinterpret_cast<WPARAM>(theme::FontIconLarge()), TRUE);
        InvalidateRect(g.hintIcon, nullptr, TRUE);
    }
}

// ---------------------------------------------------------------- 文案与语言
// 列的顺序与宽度固定，只有列名随语言变
struct ColDef {
    int  width;
    Str  name;
};
const ColDef kFileCols[kFileColumns] = {
    { 430, Str::ColPath }, { 90, Str::ColSize }, { 190, Str::ColOwner }, { 130, Str::ColStatus } };
const ColDef kProcCols[kProcColumns] = {
    { 220, Str::ColProcess }, { 70, Str::ColPid }, { 260, Str::ColDesc },
    { 60, Str::ColHandles },  { 90, Str::ColStatus } };

std::wstring IdleStatusText() {
    if (g.failClosed) {
        return T(Str::StatusFailClosed);
    }
    if (g.elevated) {
        return T(Str::StatusIdleElevated);
    }
    return T(Str::StatusIdle);
}

void SetColumnTexts(HWND list, const ColDef* defs, int count) {
    if (!list) return;
    for (int i = 0; i < count; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT;
        c.iSubItem = i;
        c.pszText = const_cast<wchar_t*>(T(defs[i].name));
        ListView_SetColumn(list, i, &c);
    }
}

// 切换语言后重建所有可见文案：控件文字 → 列名 → 空态/横幅 → 状态 → 列表内容 → 重排
void ApplyLanguage(HWND hwnd) {
    struct BtnText {
        HWND h;
        Str  id;
    };
    const BtnText texts[] = {
        { g.btnAddFile, Str::BtnAddFile },  { g.btnAddDir, Str::BtnAddFolder },
        { g.btnDeep, Str::BtnAdminScan },   { g.chkForce, Str::BtnForce },
        { g.btnUnlock, Str::BtnUnlock },    { g.btnDelete, Str::BtnUnlockDelete },
        { g.btnRename, Str::BtnUnlockRename }, { g.btnMove, Str::BtnUnlockMove },
        { g.btnCopy, Str::BtnUnlockCopy },  { g.btnRescan, Str::BtnRescan },
        { g.btnMin, Str::BtnMinimize },     { g.btnMax, Str::BtnMaximize },
        { g.btnClose, Str::BtnClose },      { g.btnSettings, Str::BtnSettings },
    };
    for (const auto& t : texts) {
        if (t.h) SetWindowTextW(t.h, T(t.id));
    }

    SetColumnTexts(g.listFiles, kFileCols, kFileColumns);
    SetColumnTexts(g.listProcs, kProcCols, kProcColumns);

    if (g.hint) {
        SetWindowTextW(g.hint, T(Str::HintDrop));
    }

    g.bannerVisible = g.failClosed || g.elevated;
    if (g.failClosed) {
        g.bannerText = T(Str::BannerFailClosed);
    } else if (g.elevated) {
        g.bannerText = T(Str::BannerElevated);
    }

    if (!g.busy) SetStatus(IdleStatusText());

    RefreshFileList();
    RefreshProcessList();
    Layout(hwnd);
    if (hwnd) InvalidateRect(hwnd, nullptr, TRUE);
}

// ---------------------------------------------------------------- 设置窗口
// 独立的弹出窗口：自绘标题栏（和主窗口同一套观感）+ 一个原生下拉框。
// 选择即生效（写注册表并刷新主窗口），关掉窗口不会再回滚。
// 普通带标题栏的对话框：自定义标题栏在这个弹窗上拿不到 WM_NCCALCSIZE，
// 强行做会被系统边框切掉一圈，直接用系统标题栏（深色由 DWM 属性负责）
constexpr DWORD kSettingsStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
constexpr DWORD kSettingsExStyle = WS_EX_DLGMODALFRAME;

// 设置窗口的内容尺寸（96 DPI 逻辑值）：外观 + 语言 + 右键菜单 + 分隔线 + 关于区
constexpr int kSettingsContentW = 400;
constexpr int kSettingsContentH = 346;

// 外观下拉的三项，顺序与 theme::Mode 一致
const Str kThemeItems[3] = { Str::ThemeSystem, Str::ThemeLight, Str::ThemeDark };

HWND  g_settingsWnd = nullptr;
HWND  g_settingsOwner = nullptr;
HWND  g_settingsAppearance = nullptr;
HWND  g_settingsCombo = nullptr;
HWND  g_settingsLink = nullptr;
HWND  g_settingsShell = nullptr;
HICON g_settingsIcon = nullptr;
RECT  g_setAppLabelRc{};
RECT  g_setAppComboRc{};
RECT  g_setLabelRc{};
RECT  g_setComboRc{};
RECT  g_setHintRc{};
RECT  g_setShellRc{};
RECT  g_setShellHintRc{};
RECT  g_setHeadRc{};
RECT  g_setIconRc{};
RECT  g_setNameRc{};
RECT  g_setLicenseRc{};
RECT  g_setLinkRc{};
int   g_setDividerY = 0;

void LayoutSettings(HWND hwnd) {
    if (!hwnd) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int pad = theme::Scale(18);

    // 外观：跟随系统 / 浅色 / 深色
    g_setAppLabelRc = { pad, theme::Scale(14), rc.right - pad, theme::Scale(36) };
    g_setAppComboRc = { pad, g_setAppLabelRc.bottom + theme::Scale(6), rc.right - pad, 0 };
    if (g_settingsAppearance) {
        MoveWindow(g_settingsAppearance, g_setAppComboRc.left, g_setAppComboRc.top,
                   g_setAppComboRc.right - g_setAppComboRc.left, theme::Scale(200), TRUE);
    }

    // 语言区
    const int langTop = g_setAppComboRc.top + theme::Scale(34);
    g_setLabelRc = { pad, langTop, rc.right - pad, langTop + theme::Scale(22) };
    g_setComboRc = { pad, g_setLabelRc.bottom + theme::Scale(6), rc.right - pad, 0 };
    g_setHintRc  = { pad, g_setComboRc.top + theme::Scale(34), rc.right - pad,
                     g_setComboRc.top + theme::Scale(68) };

    if (g_settingsCombo) {
        MoveWindow(g_settingsCombo, g_setComboRc.left, g_setComboRc.top,
                   g_setComboRc.right - g_setComboRc.left, theme::Scale(200), TRUE);
    }

    // 右键菜单开关：复选框 + 一行说明（说明文字与复选框文字左对齐）
    const int shellTop = g_setHintRc.bottom + theme::Scale(10);
    g_setShellRc = { pad, shellTop, rc.right - pad, shellTop + theme::Scale(24) };
    const int shellTextX = pad + theme::ControlPadX() + theme::Scale(16) + theme::Scale(8);
    g_setShellHintRc = { shellTextX, g_setShellRc.bottom + theme::Scale(2), rc.right - pad,
                         g_setShellRc.bottom + theme::Scale(18) };
    if (g_settingsShell) {
        MoveWindow(g_settingsShell, g_setShellRc.left, g_setShellRc.top,
                   g_setShellRc.right - g_setShellRc.left,
                   g_setShellRc.bottom - g_setShellRc.top, TRUE);
    }

    // 关于区：一条分隔线，左边 30px 图标，右边三行（名称+版本 / 许可证 / 仓库链接）
    g_setDividerY = g_setShellHintRc.bottom + theme::Scale(10);
    const int headTop = g_setDividerY + theme::Scale(12);
    g_setHeadRc = { pad, headTop, rc.right - pad, headTop + theme::Scale(16) };

    const int iconSize = theme::Scale(30);
    const int blockTop = headTop + theme::Scale(24);
    g_setIconRc = { pad, blockTop, pad + iconSize, blockTop + iconSize };

    const int textLeft = g_setIconRc.right + theme::Scale(12);
    g_setNameRc    = { textLeft, blockTop - theme::Scale(2), rc.right - pad,
                       blockTop + theme::Scale(20) };
    g_setLicenseRc = { textLeft, blockTop + theme::Scale(20), rc.right - pad,
                       blockTop + theme::Scale(38) };
    g_setLinkRc    = { textLeft, blockTop + theme::Scale(40), rc.right - pad,
                       blockTop + theme::Scale(62) };

    if (g_settingsLink) {
        MoveWindow(g_settingsLink, g_setLinkRc.left, g_setLinkRc.top,
                   g_setLinkRc.right - g_setLinkRc.left,
                   g_setLinkRc.bottom - g_setLinkRc.top, TRUE);
    }
}

void PaintSettings(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    theme::FillSolid(dc, rc, theme::Color(theme::Token::WindowBg));

    RECT al = g_setAppLabelRc;
    theme::DrawTextLine(dc, T(Str::SettingsAppearance), al, theme::FontCaption(),
                        theme::Color(theme::Token::TextSecondary),
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT lr = g_setLabelRc;
    theme::DrawTextLine(dc, T(Str::SettingsLanguage), lr, theme::FontCaption(),
                        theme::Color(theme::Token::TextSecondary),
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT hr = g_setHintRc;
    theme::DrawTextLine(dc, T(Str::SettingsHint), hr, theme::FontCaption(),
                        theme::Color(theme::Token::TextTertiary),
                        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    theme::DrawTextLine(dc, T(Str::SettingsShellMenuHint), g_setShellHintRc, theme::FontCaption(),
                        theme::Color(theme::Token::TextTertiary),
                        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    // 分隔线：设置区 / 关于区
    const int pad = theme::Scale(18);
    RECT line{ pad, g_setDividerY, rc.right - pad, g_setDividerY + theme::Scale(1) };
    theme::FillSolid(dc, line, theme::Color(theme::Token::Divider));

    // 关于：图标 + 名称与版本 + 许可证（链接是自绘按钮，见 WM_DRAWITEM）
    theme::DrawTextLine(dc, T(Str::SettingsAbout), g_setHeadRc, theme::FontCaption(),
                        theme::Color(theme::Token::TextSecondary),
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (g_settingsIcon) {
        DrawIconEx(dc, g_setIconRc.left, g_setIconRc.top, g_settingsIcon,
                   g_setIconRc.right - g_setIconRc.left, g_setIconRc.bottom - g_setIconRc.top,
                   0, nullptr, DI_NORMAL);
    }

    theme::DrawTextLine(dc, kAppName, g_setNameRc, theme::FontStrong(),
                        theme::Color(theme::Token::TextPrimary),
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    // 版本紧跟在名称后面，字号降一档、颜色降一级，读起来是同一行的两段信息
    const int nameW = theme::MeasureText(dc, kAppName, theme::FontStrong());
    RECT vr{ g_setNameRc.left + nameW + theme::Scale(8), g_setNameRc.top,
             g_setNameRc.right, g_setNameRc.bottom };
    theme::DrawTextLine(dc, std::wstring(L"v") + kAppVersion, vr, theme::FontCaption(),
                        theme::Color(theme::Token::TextTertiary),
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    theme::DrawTextLine(dc, T(Str::SettingsLicense), g_setLicenseRc, theme::FontCaption(),
                        theme::Color(theme::Token::TextSecondary),
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// 主题变化（手动切换 / 系统深浅色变化 / DPI 变化）后统一重绘：
// 调色板 + 字体 + 子控件主题 + 两个窗口的布局都要走一遍，少一步就会出现半深半浅
void ApplyThemeChange() {
    if (!g.hwnd) return;
    theme::Refresh(g.hwnd);          // 内含 ApplyWindowFrame
    ApplyThemeToControls();
    Layout(g.hwnd);
    // 自绘按钮/列表是独立子窗口，只失效父窗口不会让它们重画 —— 必须带上 RDW_ALLCHILDREN
    RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);

    if (g_settingsWnd) {
        theme::ApplyWindowFrame(g_settingsWnd);
        const wchar_t* cls = theme::IsDark() ? L"DarkMode_CFD" : nullptr;
        theme::SetThemeClass(g_settingsCombo, cls);
        theme::SetThemeClass(g_settingsAppearance, cls);
        // theme::Refresh 会把旧字体删掉重建：控件手里握着的是已销毁的 HFONT，
        // 不重新 WM_SETFONT 就会退回系统默认字体（下拉文字忽大忽小的根因）
        for (HWND combo : { g_settingsAppearance, g_settingsCombo }) {
            if (combo) SendMessageW(combo, WM_SETFONT,
                                    reinterpret_cast<WPARAM>(theme::FontBody()), TRUE);
        }
        LayoutSettings(g_settingsWnd);
        RedrawWindow(g_settingsWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

void ApplySettingsLanguage() {
    SetLang((Lang)SendMessageW(g_settingsCombo, CB_GETCURSEL, 0, 0));
    SaveLangPreference(GetLang());
    if (g.hwnd) ApplyLanguage(g.hwnd);
    if (g_settingsWnd) {
        SetWindowTextW(g_settingsWnd, T(Str::SettingsTitle));
        if (g_settingsShell) SetWindowTextW(g_settingsShell, T(Str::SettingsShellMenu));
        // 下拉项是运行时文案，切语言要重填（选中项保持不变）
        if (g_settingsAppearance) {
            const int sel = static_cast<int>(theme::GetMode());
            SendMessageW(g_settingsAppearance, CB_RESETCONTENT, 0, 0);
            for (const Str id : kThemeItems) {
                SendMessageW(g_settingsAppearance, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(T(id)));
            }
            SendMessageW(g_settingsAppearance, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
        }
        // 菜单文字是安装时写进注册表的，语言变了要重写一次，否则右键里还是旧语言
        if (shell::IsInstalled()) shell::Install();
        LayoutSettings(g_settingsWnd);
        InvalidateRect(g_settingsWnd, nullptr, TRUE);
    }
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_settingsWnd = hwnd;
            const HINSTANCE inst = g.inst;

            // 外观：跟随系统 / 浅色 / 深色
            g_settingsAppearance = CreateWindowExW(
                0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, 10,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SET_APPEAR)), inst, nullptr);
            for (const Str id : kThemeItems) {
                SendMessageW(g_settingsAppearance, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(T(id)));
            }
            SendMessageW(g_settingsAppearance, CB_SETCURSEL,
                         static_cast<WPARAM>(theme::GetMode()), 0);
            SendMessageW(g_settingsAppearance, WM_SETFONT,
                         reinterpret_cast<WPARAM>(theme::FontBody()), TRUE);
            theme::SetThemeClass(g_settingsAppearance, theme::IsDark() ? L"DarkMode_CFD" : nullptr);

            g_settingsCombo = CreateWindowExW(
                0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, 10,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SET_LANG)), inst, nullptr);
            for (int i = 0; i < static_cast<int>(Lang::Count); ++i) {
                SendMessageW(g_settingsCombo, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(LangNativeName(static_cast<Lang>(i))));
            }
            SendMessageW(g_settingsCombo, CB_SETCURSEL, static_cast<WPARAM>(GetLang()), 0);
            SendMessageW(g_settingsCombo, WM_SETFONT,
                         reinterpret_cast<WPARAM>(theme::FontBody()), TRUE);
            theme::SetThemeClass(g_settingsCombo, theme::IsDark() ? L"DarkMode_CFD" : nullptr);

            // 仓库链接：自绘按钮，既保留 UIA Name（窗口文字就是 URL）与键盘焦点，
            // 又能用主题强调色画，不必迁就原生 SysLink 的固定蓝色
            g_settingsLink = CreateWindowExW(
                0, L"BUTTON", kRepoUrl,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SET_LINK)), inst, nullptr);
            if (g_settingsLink) SetWindowSubclass(g_settingsLink, ButtonSubclassProc, 1, 0);

            // 右键菜单开关：自绘复选框（和主窗口「强制模式」同一套画法）
            g_settingsShell = CreateWindowExW(
                0, L"BUTTON", T(Str::SettingsShellMenu),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SET_SHELL)), inst, nullptr);
            if (g_settingsShell) SetWindowSubclass(g_settingsShell, ButtonSubclassProc, 1, 0);

            // 关于区的图标：按目标尺寸取最近的一档，DrawIconEx 再按当前 DPI 缩放
            g_settingsIcon = static_cast<HICON>(LoadImageW(
                inst, MAKEINTRESOURCEW(101), IMAGE_ICON, theme::Scale(30), theme::Scale(30),
                LR_DEFAULTCOLOR));

            LayoutSettings(hwnd);
            return 0;
        }

        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            theme::FillSolid(reinterpret_cast<HDC>(wParam), rc, theme::Color(theme::Token::WindowBg));
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            PaintSettings(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis && dis->CtlID == ID_SET_SHELL) {
                theme::ButtonPaint p;
                p.rc       = dis->rcItem;
                p.checkbox = true;
                p.flat     = true;   // 设置窗口是纯色背景，不要再套一层白卡片
                p.checked  = shell::IsInstalled();
                p.disabled = (dis->itemState & ODS_DISABLED) != 0;
                p.focused  = (dis->itemState & ODS_FOCUS) != 0 && !p.disabled;
                wchar_t label[128]{};
                GetWindowTextW(dis->hwndItem, label, ARRAYSIZE(label));
                p.text = label;
                auto hot = g.btnHot.find(dis->hwndItem);
                p.hot = hot != g.btnHot.end() && hot->second;
                theme::PaintButton(dis->hDC, p);
                return TRUE;
            }
            if (dis && dis->CtlID == ID_SET_LINK) {
                theme::ButtonPaint p;
                p.rc       = dis->rcItem;
                p.link     = true;
                p.text     = kRepoUrl;
                p.disabled = (dis->itemState & ODS_DISABLED) != 0;
                p.pressed  = (dis->itemState & ODS_SELECTED) != 0;
                p.focused  = (dis->itemState & ODS_FOCUS) != 0 && !p.disabled;
                auto hot = g.btnHot.find(dis->hwndItem);
                p.hot = hot != g.btnHot.end() && hot->second;
                theme::PaintButton(dis->hDC, p);
                return TRUE;
            }
            return FALSE;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == ID_SET_LANG && HIWORD(wParam) == CBN_SELCHANGE) {
                ApplySettingsLanguage();
                return 0;
            }
            if (id == ID_SET_LINK) {
                ShellExecuteW(hwnd, L"open", kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (id == ID_SET_APPEAR && HIWORD(wParam) == CBN_SELCHANGE) {
                const int sel = static_cast<int>(SendMessageW(g_settingsAppearance, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < 3) {
                    theme::SetMode(static_cast<theme::Mode>(sel));
                    ApplyThemeChange();
                }
                return 0;
            }
            if (id == ID_SET_SHELL) {
                const bool installed = shell::IsInstalled();
                const bool ok = installed ? shell::Uninstall() : shell::Install();
                if (!ok) {
                    MessageBoxW(hwnd, T(Str::ShellInstallFailed), L"Unlocker",
                                MB_ICONWARNING | MB_OK);
                }
                shell::MarkPromptAnswered();   // 手动开关过，就别再弹首次询问
                if (g_settingsShell) InvalidateRect(g_settingsShell, nullptr, TRUE);
                return 0;
            }
            if (id == IDCANCEL) {   // IsDialogMessage 把 Esc 翻成 IDCANCEL
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }

        case WM_DESTROY:
            g_settingsWnd = nullptr;
            g_settingsCombo = nullptr;
            g_settingsLink = nullptr;
            if (g_settingsIcon) {
                DestroyIcon(g_settingsIcon);
                g_settingsIcon = nullptr;
            }
            if (g_settingsOwner && IsWindow(g_settingsOwner)) {
                EnableWindow(g_settingsOwner, TRUE);
                SetForegroundWindow(g_settingsOwner);
            }
            g_settingsOwner = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OpenSettings(HWND parent) {
    if (g_settingsWnd) {
        SetForegroundWindow(g_settingsWnd);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = SettingsProc;
        wc.hInstance     = g.inst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"UnlockerSettings";
        wc.hIcon         = static_cast<HICON>(LoadImageW(g.inst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                         GetSystemMetrics(SM_CXICON),
                                                         GetSystemMetrics(SM_CYICON), 0));
        wc.hIconSm       = static_cast<HICON>(LoadImageW(g.inst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                         GetSystemMetrics(SM_CXSMICON),
                                                         GetSystemMetrics(SM_CYSMICON), 0));
        RegisterClassExW(&wc);
        registered = true;
    }

    // 客户区尺寸固定（语言区 + 关于区）；窗口尺寸交给 AdjustWindowRectEx 补上标题栏与边框
    const int contentW = theme::Scale(kSettingsContentW);
    const int contentH = theme::Scale(kSettingsContentH);
    RECT frame{ 0, 0, contentW, contentH };
    AdjustWindowRectEx(&frame, kSettingsStyle, FALSE, kSettingsExStyle);
    const int w = frame.right - frame.left;
    const int h = frame.bottom - frame.top;
    RECT pr{};
    GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - w) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - h) / 3;

    g_settingsOwner = parent;
    // WS_THICKFRAME 是必需的：没有它，DWM 会强行保留系统边框（客户区被切掉一圈），
    // NCCALCSIZE 返回 0 也不生效；缩放行为由上面的 WM_NCHITTEST 关掉
    HWND hwnd = CreateWindowExW(kSettingsExStyle, L"UnlockerSettings", T(Str::SettingsTitle),
                                kSettingsStyle, x, y, w, h, parent, nullptr, g.inst, nullptr);
    if (!hwnd) {
        g_settingsOwner = nullptr;
        return;
    }
    theme::ApplyWindowFrame(hwnd);
    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(hwnd);
}

// ---------------------------------------------------------------- 首次启动询问
// 只问一次：装或不装都记下来，之后由「设置 → 资源管理器右键菜单」控制。
// 和设置窗口同一套观感：系统标题栏 + 自绘内容 + 两个自绘按钮。
constexpr int kPromptContentW = 400;
constexpr int kPromptContentH = 132;
constexpr DWORD kPromptStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
constexpr DWORD kPromptExStyle = WS_EX_DLGMODALFRAME;

HWND g_promptWnd = nullptr;
HWND g_promptOwner = nullptr;
HWND g_promptAdd = nullptr;
HWND g_promptLater = nullptr;
RECT g_promptBodyRc{};
std::wstring g_promptText;

void LayoutPrompt(HWND hwnd) {
    if (!hwnd) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int pad = theme::Scale(18);
    const int btnH = theme::Scale(30);
    const int gap = theme::Scale(8);

    g_promptBodyRc = { pad, pad, rc.right - pad, rc.bottom - pad - btnH - theme::Scale(14) };

    HDC dc = GetDC(hwnd);
    const int addW = ButtonWidth(dc, g_promptAdd, nullptr);
    const int laterW = ButtonWidth(dc, g_promptLater, nullptr);
    ReleaseDC(hwnd, dc);

    const int y = rc.bottom - pad - btnH;
    MoveWindow(g_promptAdd, rc.right - pad - addW, y, addW, btnH, TRUE);
    MoveWindow(g_promptLater, rc.right - pad - addW - gap - laterW, y, laterW, btnH, TRUE);
}

void PaintPrompt(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    theme::FillSolid(dc, rc, theme::Color(theme::Token::WindowBg));
    theme::DrawTextLine(dc, g_promptText, g_promptBodyRc, theme::FontBody(),
                        theme::Color(theme::Token::TextPrimary),
                        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
}

void ClosePrompt() {
    if (g_promptWnd) DestroyWindow(g_promptWnd);
}

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_promptWnd = hwnd;
            const HINSTANCE inst = g.inst;
            g_promptLater = CreateWindowExW(
                0, L"BUTTON", T(Str::ShellPromptLater),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PROMPT_LATER)), inst, nullptr);
            g_promptAdd = CreateWindowExW(
                0, L"BUTTON", T(Str::ShellPromptAdd),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PROMPT_ADD)), inst, nullptr);
            if (g_promptLater) SetWindowSubclass(g_promptLater, ButtonSubclassProc, 1, 0);
            if (g_promptAdd) SetWindowSubclass(g_promptAdd, ButtonSubclassProc, 1, 0);

            g_promptText = Tf(Str::ShellPromptBody, T(Str::ShellMenuVerb));
            LayoutPrompt(hwnd);
            return 0;
        }

        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            theme::FillSolid(reinterpret_cast<HDC>(wParam), rc, theme::Color(theme::Token::WindowBg));
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            PaintPrompt(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || dis->CtlType != ODT_BUTTON) return FALSE;
            theme::ButtonPaint p;
            p.rc       = dis->rcItem;
            p.primary  = dis->CtlID == ID_PROMPT_ADD;
            p.disabled = (dis->itemState & ODS_DISABLED) != 0;
            p.pressed  = (dis->itemState & ODS_SELECTED) != 0;
            p.focused  = (dis->itemState & ODS_FOCUS) != 0 && !p.disabled;
            wchar_t label[64]{};
            GetWindowTextW(dis->hwndItem, label, ARRAYSIZE(label));
            p.text = label;
            auto hot = g.btnHot.find(dis->hwndItem);
            p.hot = hot != g.btnHot.end() && hot->second;
            theme::PaintButton(dis->hDC, p);
            return TRUE;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == ID_PROMPT_ADD || id == IDOK) {
                if (!shell::Install()) {
                    MessageBoxW(hwnd, T(Str::ShellInstallFailed), L"Unlocker",
                                MB_ICONWARNING | MB_OK);
                }
                shell::MarkPromptAnswered();
                ClosePrompt();
                return 0;
            }
            if (id == ID_PROMPT_LATER || id == IDCANCEL) {
                shell::MarkPromptAnswered();
                ClosePrompt();
                return 0;
            }
            return 0;
        }

        case WM_CLOSE:
            shell::MarkPromptAnswered();   // 关掉窗口等同于「暂不」
            ClosePrompt();
            return 0;

        case WM_DESTROY:
            g_promptWnd = nullptr;
            g_promptAdd = nullptr;
            g_promptLater = nullptr;
            if (g_promptOwner && IsWindow(g_promptOwner)) {
                EnableWindow(g_promptOwner, TRUE);
                SetForegroundWindow(g_promptOwner);
            }
            g_promptOwner = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowShellMenuPrompt(HWND parent) {
    if (g_promptWnd) return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = PromptProc;
        wc.hInstance     = g.inst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"UnlockerPrompt";
        wc.hIcon         = static_cast<HICON>(LoadImageW(g.inst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                         GetSystemMetrics(SM_CXICON),
                                                         GetSystemMetrics(SM_CYICON), 0));
        wc.hIconSm       = static_cast<HICON>(LoadImageW(g.inst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                         GetSystemMetrics(SM_CXSMICON),
                                                         GetSystemMetrics(SM_CYSMICON), 0));
        RegisterClassExW(&wc);
        registered = true;
    }

    const int contentW = theme::Scale(kPromptContentW);
    const int contentH = theme::Scale(kPromptContentH);
    RECT frame{ 0, 0, contentW, contentH };
    AdjustWindowRectEx(&frame, kPromptStyle, FALSE, kPromptExStyle);
    const int w = frame.right - frame.left;
    const int h = frame.bottom - frame.top;
    RECT pr{};
    GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - w) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - h) / 3;

    g_promptOwner = parent;
    HWND hwnd = CreateWindowExW(kPromptExStyle, L"UnlockerPrompt", T(Str::ShellPromptTitle),
                                kPromptStyle, x, y, w, h, parent, nullptr, g.inst, nullptr);
    if (!hwnd) {
        g_promptOwner = nullptr;
        return;
    }
    theme::ApplyWindowFrame(hwnd);
    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    if (g_promptAdd) SetFocus(g_promptAdd);
}

// ---------------------------------------------------------------- 创建控件
void CreateControls(HWND hwnd) {
    const HINSTANCE inst = g.inst;

    auto makeButton = [&](const wchar_t* text, int id, bool tabstop) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                 WS_CHILD | WS_VISIBLE | (tabstop ? WS_TABSTOP : 0) | BS_OWNERDRAW,
                                 0, 0, 10, 10, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        if (h) SetWindowSubclass(h, ButtonSubclassProc, 1, 0);
        return h;
    };

    // 文字统一由 ApplyLanguage 填，这里只建空壳，避免两处各写一份文案
    g.btnAddFile = makeButton(L"", ID_ADD_FILE, true);
    g.btnAddDir  = makeButton(L"", ID_ADD_DIR, true);
    g.btnDeep    = makeButton(L"", ID_DEEP_SCAN, true);
    g.chkForce   = makeButton(L"", ID_FORCE, true);
    g.btnSettings = makeButton(L"", ID_SETTINGS, true);

    g.btnMin   = makeButton(L"", ID_MINIMIZE, false);
    g.btnMax   = makeButton(L"", ID_MAXIMIZE, false);
    g.btnClose = makeButton(L"", ID_CLOSE, false);

    const DWORD lvStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS |
                          LVS_OWNERDRAWFIXED;

    g.listFiles = CreateWindowExW(0, WC_LISTVIEWW, L"", lvStyle, 0, 0, 10, 10, hwnd, nullptr, inst,
                                  nullptr);
    ListView_SetExtendedListViewStyle(g.listFiles,
                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    for (int i = 0; i < kFileColumns; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<wchar_t*>(T(kFileCols[i].name));
        c.cx = theme::Scale(kFileCols[i].width);
        c.iSubItem = i;
        ListView_InsertColumn(g.listFiles, i, &c);
    }

    g.listProcs = CreateWindowExW(0, WC_LISTVIEWW, L"", lvStyle, 0, 0, 10, 10, hwnd, nullptr, inst,
                                  nullptr);
    ListView_SetExtendedListViewStyle(g.listProcs,
                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (int i = 0; i < kProcColumns; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<wchar_t*>(T(kProcCols[i].name));
        c.cx = theme::Scale(kProcCols[i].width);
        c.iSubItem = i;
        ListView_InsertColumn(g.listProcs, i, &c);
    }

    g.hdrFiles = reinterpret_cast<HWND>(SendMessageW(g.listFiles, LVM_GETHEADER, 0, 0));
    g.hdrProcs = reinterpret_cast<HWND>(SendMessageW(g.listProcs, LVM_GETHEADER, 0, 0));

    // 空态：大号文件夹图示 + 两行提示（SS_CENTERIMAGE 会强制单行，所以用 SS_CENTER）
    g.hintIcon = CreateWindowExW(0, L"STATIC", L"\xE8B7", WS_CHILD | WS_VISIBLE | SS_CENTER,
                                 0, 0, 10, 10, hwnd, nullptr, inst, nullptr);
    g.hint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 10, 10,
                             hwnd, nullptr, inst, nullptr);

    g.btnUnlock = makeButton(L"", ID_UNLOCK, true);
    g.btnDelete = makeButton(L"", ID_DELETE, true);
    g.btnRename = makeButton(L"", ID_RENAME, true);
    g.btnMove   = makeButton(L"", ID_MOVE, true);
    g.btnCopy   = makeButton(L"", ID_COPY, true);
    g.btnRescan = makeButton(L"", ID_RESCAN, true);

    ApplyThemeToControls();
    ApplyLanguage(hwnd);   // 文案 + 首次 Layout

    DragAcceptFiles(hwnd, TRUE);   // 方案 B 的收益：中完整性窗口与资源管理器同级，UIPI 完全不拦
    StartUiPipeListener(hwnd);
}

// ---------------------------------------------------------------- 对话框
void ShowDeniedDialog(const UiItem& it) {
    std::wstring text = it.rule.empty() ? T(Str::DlgDeniedTitle)
                                        : (it.rule + T(Str::DlgDeniedSuffix));
    text += L"\n";
    text += it.canonical.empty() ? it.path : it.canonical;
    text += L"\n\n";
    text += it.reason;
    text += T(Str::DlgDeniedFooter);
    MessageBoxW(g.hwnd, text.c_str(), L"Unlocker", MB_ICONWARNING | MB_OK);
}

// ---------------------------------------------------------------- 窗口过程
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g.hwnd = hwnd;
            g.elevated = IsUiElevated();
            if (g.elevated) PatchDropForElevatedUi(hwnd);
            theme::Init(hwnd);
            CreateControls(hwnd);
            Layout(hwnd);
            UpdateButtons();
            SetStatus(IdleStatusText());
            return 0;

        // ---- 自绘标题栏：吃掉整个非客户区（含最大化补偿）
        case WM_NCCALCSIZE: {
            if (wParam != TRUE) break;
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            if (GetWindowPlacement(hwnd, &wp) && wp.showCmd == SW_SHOWMAXIMIZED) {
                // 最大化时客户区会被推到屏幕外 SM_CXSIZEFRAME 那么宽，必须按工作区还原
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                    params->rgrc[0] = mi.rcWork;
                }
            }
            return 0;
        }

        case WM_NCHITTEST: {
            const LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
            if (hit != HTCLIENT) return hit;   // 边框仍由系统处理缩放
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            if (pt.y >= 0 && pt.y < theme::Scale(32) && pt.x >= 0 && pt.x < g.titleRc.right) {
                return HTCAPTION;   // 拖拽 / 双击最大化 / 右键系统菜单都靠它
            }
            return HTCLIENT;
        }

        case WM_NCACTIVATE:
            if (g.hwnd) InvalidateRect(hwnd, &g.titleRc, FALSE);
            return TRUE;

        case WM_ACTIVATE:
            if (g.hwnd) InvalidateRect(hwnd, &g.titleRc, FALSE);
            break;

        case WM_GETMINMAXINFO: {
            DefWindowProcW(hwnd, msg, wParam, lParam);
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = theme::Scale(720);
            mmi->ptMinTrackSize.y = theme::Scale(520);
            return 0;
        }

        case WM_SIZE:
            Layout(hwnd);
            if (g.btnMax) {
                SetWindowTextW(g.btnMax, wParam == SIZE_MAXIMIZED ? L"还原" : L"最大化");
                InvalidateRect(g.btnMax, nullptr, FALSE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            theme::FillSolid(reinterpret_cast<HDC>(wParam), rc, theme::Color(theme::Token::WindowBg));
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            theme::FillSolid(dc, rc, theme::Color(theme::Token::WindowBg));
            PaintTitleBar(dc);
            PaintBanner(dc);
            PaintCards(dc);
            PaintStatus(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis) return FALSE;
            if (dis->CtlType == ODT_BUTTON) {
                DrawOwnerButton(dis);
                return TRUE;
            }
            if (dis->CtlType == ODT_LISTVIEW) {
                if (dis->hwndItem == g.listFiles) DrawFileRow(dis);
                else DrawProcRow(dis);
                return TRUE;
            }
            return FALSE;
        }

        case WM_MEASUREITEM: {
            auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (mis && mis->CtlType == ODT_LISTVIEW) {
                mis->itemHeight = theme::Scale(32);
                return TRUE;
            }
            return FALSE;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, theme::Color(theme::Token::TextTertiary));
            SetBkColor(dc, theme::Color(theme::Token::CardBg));
            return reinterpret_cast<LRESULT>(theme::Brush(theme::Token::CardBg));
        }

        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyThemeChange();
            return 0;
        }

        case WM_SETTINGCHANGE: {
            const wchar_t* area = reinterpret_cast<const wchar_t*>(lParam);
            if (area && (wcscmp(area, L"ImmersiveColorSet") == 0 ||
                         wcscmp(area, L"WindowMetrics") == 0)) {
                ApplyThemeChange();
            }
            return 0;
        }

        case WM_DROPFILES: {
            HDROP hdrop = reinterpret_cast<HDROP>(wParam);
            const UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> paths;
            for (UINT i = 0; i < n; ++i) {
                const UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
                std::wstring p(len + 1, L'\0');
                DragQueryFileW(hdrop, i, p.data(), len + 1);
                p.resize(len);
                paths.push_back(p);
            }
            DragFinish(hdrop);
            AddPaths(paths);
            return 0;
        }

        case WM_APP_SHELL_PATHS: {
            auto* paths = reinterpret_cast<std::vector<std::wstring>*>(lParam);
            if (paths) {
                AddPaths(*paths);
                delete paths;
            }
            return 0;
        }

        case WM_APP_STATUS: {
            std::unique_ptr<StatusMsg> m(reinterpret_cast<StatusMsg*>(lParam));
            if (m && !m->text.empty()) SetStatus(m->text);
            return 0;
        }

        case WM_APP_SCAN_DONE:
            g.busy = false;
            UpdateButtons();
            return 0;

        case WM_APP_SCAN_ITEM: {
            std::unique_ptr<ScanMsg> m(reinterpret_cast<ScanMsg*>(lParam));
            if (!m) return 0;
            UiItem* it = FindItem(m->id);
            if (!it) return 0;
            if (m->remove) {
                // NotFound 是终止态：直接移除该行
                for (size_t i = 0; i < g.items.size(); ++i) {
                    if (g.items[i].id == m->id) { g.items.erase(g.items.begin() + i); break; }
                }
            } else {
                UiItem merged = m->item;
                merged.path = it->path;
                merged.canonical = it->canonical.empty() ? m->item.canonical : it->canonical;
                merged.verdict = it->verdict;
                merged.rule = it->rule;
                merged.reason = it->reason;
                *it = std::move(merged);
            }
            RefreshFileList();
            RefreshProcessList();
            return 0;
        }

        case WM_APP_OP_DONE: {
            std::unique_ptr<OpDone> done(reinterpret_cast<OpDone*>(lParam));
            if (!done) return 0;
            if (wParam == 2) {   // 深扫失败
                g.busy = false;
                UpdateButtons();
                SetStatus(done->summary);
                MessageBoxW(hwnd, done->summary.c_str(), L"Unlocker", MB_ICONWARNING | MB_OK);
                return 0;
            }
            ApplyOpResult(*done);
            return 0;
        }

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (!hdr) return 0;

            if (hdr->code == NM_CUSTOMDRAW && (hdr->hwndFrom == g.listFiles || hdr->hwndFrom == g.listProcs)) {
                auto* cd = reinterpret_cast<LPNMCUSTOMDRAW>(lParam);
                if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYPOSTPAINT;
                if (cd->dwDrawStage == CDDS_POSTPAINT) {
                    FillListTail(hdr->hwndFrom, cd->hdc);
                    return CDRF_DODEFAULT;
                }
                return CDRF_DODEFAULT;
            }

            if (hdr->idFrom == reinterpret_cast<UINT_PTR>(g.listFiles) && hdr->code == LVN_ITEMCHANGED) {
                RefreshProcessList();
                return 0;
            }
            if (hdr->idFrom == reinterpret_cast<UINT_PTR>(g.listFiles) && hdr->code == NM_DBLCLK) {
                auto* nv = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                if (nv->iItem >= 0 && static_cast<size_t>(nv->iItem) < g.items.size()) {
                    const auto& it = g.items[nv->iItem];
                    if (it.verdict == Verdict::Deny) ShowDeniedDialog(it);
                }
                return 0;
            }
            if (hdr->idFrom == reinterpret_cast<UINT_PTR>(g.listProcs) && hdr->code == NM_DBLCLK) {
                auto* nv = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                const int i = nv->iItem;
                if (i >= 0) {
                    // 打开进程所在目录（用户在界面上能立刻知道这是哪个软件）
                    wchar_t name[256]{};
                    ListView_GetItemText(g.listProcs, i, 0, name, ARRAYSIZE(name));
                    for (const auto& it : g.items) {
                        for (const auto& ow : it.owners) {
                            if (ow.name == name && !ow.fullPath.empty()) {
                                PIDLIST_ABSOLUTE pidl = nullptr;
                                if (SUCCEEDED(SHParseDisplayName(ow.fullPath.c_str(), nullptr, &pidl, 0, nullptr))) {
                                    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                                    CoTaskMemFree(pidl);
                                }
                                return 0;
                            }
                        }
                    }
                }
                return 0;
            }
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            switch (id) {
                case ID_ADD_FILE: {
                    auto files = PickFiles(hwnd);
                    if (!files.empty()) AddPaths(files);
                    return 0;
                }
                case ID_ADD_DIR: {
                    auto dirs = PickFolders(hwnd);
                    if (!dirs.empty()) AddPaths(dirs);
                    return 0;
                }
                case ID_DEEP_SCAN: {
                    std::vector<std::pair<uint64_t, std::wstring>> targets;
                    for (const auto& it : g.items) {
                        if (it.verdict == Verdict::Deny) continue;
                        targets.emplace_back(it.id, it.path);
                    }
                    if (targets.empty()) {
                        MessageBoxW(hwnd, T(Str::DlgNothingToScan), L"Unlocker",
                                MB_ICONINFORMATION | MB_OK);
                        return 0;
                    }
                    g.busy = true;
                    UpdateButtons();
                    StartDeepScan(targets);
                    return 0;
                }
                case ID_FORCE:
                    // BS_OWNERDRAW 按钮没有系统复选状态，自己维护并重绘
                    g.forceOn = !g.forceOn;
                    InvalidateRect(g.chkForce, nullptr, FALSE);
                    return 0;
                case ID_UNLOCK: StartOp(IpcOp::Unlock, L""); return 0;
                case ID_DELETE: StartOp(IpcOp::Delete, L""); return 0;
                case ID_RENAME: {
                    auto sel = SelectedItems();
                    if (sel.empty()) return 0;
                    const std::wstring newFull = PickNewName(hwnd, sel.front()->path);
                    if (newFull.empty()) return 0;
                    const size_t slash = newFull.find_last_of(L'\\');
                    const std::wstring name = slash == std::wstring::npos ? newFull : newFull.substr(slash + 1);
                    StartOp(IpcOp::Rename, name);
                    return 0;
                }
                case ID_MOVE: {
                    auto dirs = PickFolders(hwnd);
                    if (!dirs.empty()) StartOp(IpcOp::Move, dirs.front());
                    return 0;
                }
                case ID_COPY: {
                    auto dirs = PickFolders(hwnd);
                    if (!dirs.empty()) StartOp(IpcOp::Copy, dirs.front());
                    return 0;
                }
                case ID_RESCAN:
                    RescanAll();
                    return 0;
                case ID_SETTINGS:
                    OpenSettings(hwnd);
                    return 0;
                case ID_MINIMIZE:
                    SendMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                    return 0;
                case ID_MAXIMIZE:
                    SendMessageW(hwnd, WM_SYSCOMMAND, IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
                    return 0;
                case ID_CLOSE:
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                default:
                    return 0;
            }
        }

        case WM_CLOSE: {
            if (g.busy) {
                if (MessageBoxW(hwnd,
                                T(Str::DlgBusyQuit),
                                L"Unlocker", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                    return 0;
                }
            }
            std::lock_guard<std::mutex> lk(g.workerMutex);
            g.worker.Stop();
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int GuiMain(HINSTANCE inst, const std::vector<std::wstring>& initialPaths, int nCmdShow) {
    g.inst = inst;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    LoadLangPreference();   // 注册表记住的选择优先，否则按系统语言判断（繁体→英文）
    theme::InitProcess();   // 必须在第一个窗口出现之前：uxtheme 的进程级深色偏好只生效一次

    LogInit(L"Unlocker");
    PathResolver::Init();
    std::wstring perr;
    if (!PolicyGuard::Init(&perr) || PolicyGuard::FailClosed()) {
        g.failClosed = true;
        LogE(L"规则表自检失败，界面进入保护模式：" + perr);
    }
    HandleScanner::Init(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // 自绘：WM_ERASEBKGND 自己填，避免先刷一遍系统色
    wc.lpszClassName = L"UnlockerMainWindow";
    wc.hIcon         = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                     GetSystemMetrics(SM_CXICON),
                                                     GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm       = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                     GetSystemMetrics(SM_CXSMICON),
                                                     GetSystemMetrics(SM_CYSMICON), 0));
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Unlocker",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 980, 640,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    // 客户区即窗口区（WM_NCCALCSIZE 已去掉边框），按 DPI 换算初始尺寸
    SetWindowPos(hwnd, nullptr, 0, 0, theme::Scale(980), theme::Scale(640),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (wc.hIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    if (wc.hIconSm) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    if (!initialPaths.empty()) AddPaths(initialPaths);

    // 右键菜单：装过就顺手修正路径（exe 被移动/改名后 command 会失效），没装过且没问过就问一次
    if (shell::KeyExists()) {
        if (!shell::IsInstalled()) shell::Install();
        if (!shell::PromptAnswered()) shell::MarkPromptAnswered();
    } else if (!shell::PromptAnswered()) {
        ShowShellMenuPrompt(hwnd);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // 普通窗口不会自己处理 Tab/Shift+Tab，交给 IsDialogMessage 走 WS_TABSTOP 顺序；
        // 自绘按钮的焦点环（ODS_FOCUS）只有键盘能走到时才有意义。
        // 设置窗口是独立顶层窗口，也要走同一条规则，否则下拉框与链接之间无法用 Tab 切换
        if (g_promptWnd && (msg.hwnd == g_promptWnd || IsChild(g_promptWnd, msg.hwnd)) &&
            IsDialogMessageW(g_promptWnd, &msg)) {
            continue;
        }
        if (g_settingsWnd && (msg.hwnd == g_settingsWnd || IsChild(g_settingsWnd, msg.hwnd)) &&
            IsDialogMessageW(g_settingsWnd, &msg)) {
            continue;
        }
        if (g.hwnd && IsDialogMessageW(g.hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 收尾：等工作线程退出后再销毁共享状态，避免访问已释放内存
    if (g.opThread.joinable()) g.opThread.join();
    for (auto& t : g.scanThreads) if (t.joinable()) t.join();

    {
        std::lock_guard<std::mutex> lk(g.workerMutex);
        g.worker.Stop();
    }
    theme::Shutdown();
    CoUninitialize();
    return 0;
}

}  // namespace ul
