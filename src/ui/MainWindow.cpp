#include "ui/MainWindow.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/ErrorMap.h"
#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/FileOps.h"
#include "core/HandleScanner.h"
#include "core/PathResolver.h"
#include "core/PolicyGuard.h"
#include "ipc/Json.h"
#include "ipc/PipeClient.h"
#include "ipc/Protocol.h"
#include "ui/ElevationCheck.h"
#include "ui/FilePicker.h"
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
constexpr int ID_BANNER     = 1016;

constexpr UINT WM_APP_SCAN_ITEM = WM_APP + 1;
constexpr UINT WM_APP_SCAN_DONE = WM_APP + 2;
constexpr UINT WM_APP_OP_DONE   = WM_APP + 3;
constexpr UINT WM_APP_STATUS    = WM_APP + 4;

struct UiOwner {
    DWORD        pid = 0;
    std::wstring name, desc, fullPath;
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
    HWND      hint = nullptr;
    HWND      banner = nullptr;
    HWND      status = nullptr;
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

    std::vector<UiItem> items;
    uint64_t            nextId = 1;
    bool                elevated = false;
    bool                failClosed = false;
    bool                busy = false;
    bool                forceOn = false;
    std::wstring        statusText;

    PipeClient  worker;
    std::mutex  workerMutex;
    std::thread opThread;
    std::vector<std::thread> scanThreads;
};

App g;

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
    if (g.status) SetWindowTextW(g.status, text.c_str());
}

void PostStatus(const std::wstring& text) {
    if (!g.hwnd) return;
    auto* msg = new StatusMsg{ text };
    if (!PostMessageW(g.hwnd, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
}

std::wstring VerdictText(Verdict v) {
    switch (v) {
        case Verdict::Deny:    return L"✕ 已阻止";
        case Verdict::Confirm: return L"需确认";
        default:               return L"";
    }
}

COLORREF StateColor(OccupancyState s, bool denied) {
    if (denied) return RGB(176, 0, 0);
    switch (s) {
        case OccupancyState::Locked:         return RGB(192, 0, 0);
        case OccupancyState::ImageMapped:    return RGB(200, 96, 0);
        case OccupancyState::AccessDenied:   return RGB(150, 110, 0);
        case OccupancyState::Opened:         return RGB(110, 110, 110);
        case OccupancyState::Free:           return RGB(0, 128, 0);
        case OccupancyState::LockedByKernel: return RGB(120, 70, 150);
        case OccupancyState::NotFound:       return RGB(130, 130, 130);
        case OccupancyState::Error:          return RGB(90, 90, 90);
    }
    return RGB(0, 0, 0);
}

std::wstring OwnersSummary(const UiItem& it) {
    if (it.owners.empty()) return L"—";
    std::wstring first = it.owners.front().name;
    if (first.empty()) first = L"PID " + std::to_wstring(it.owners.front().pid);
    if (it.owners.size() > 1) first += L" (+" + std::to_wstring(it.owners.size() - 1) + L")";
    return first;
}

std::wstring StateText(const UiItem& it) {
    if (it.verdict == Verdict::Deny) return L"✕ 已阻止";
    if (!it.scanned) return it.scanning ? L"扫描中…" : L"待扫描";
    std::wstring t = OccupancyText(it.state);
    if (it.verdict == Verdict::Confirm) t = std::wstring(L"需确认 · ") + t;
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
        ListView_SetItemText(g.listFiles, static_cast<int>(i), 1,
                             const_cast<wchar_t*>((it.dir ? L"<目录>" : FormatSize(it.size)).c_str()));
        ListView_SetItemText(g.listFiles, static_cast<int>(i), 2,
                             const_cast<wchar_t*>(OwnersSummary(it).c_str()));
        ListView_SetItemText(g.listFiles, static_cast<int>(i), 3,
                             const_cast<wchar_t*>(StateText(it).c_str()));
    }
    SendMessageW(g.listFiles, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.listFiles, nullptr, TRUE);

    if (g.hint) ShowWindow(g.hint, g.items.empty() ? SW_SHOW : SW_HIDE);
}

void RefreshProcessList() {
    if (!g.listProcs) return;
    SendMessageW(g.listProcs, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g.listProcs);

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
                if (m.pid == o.pid) { m.handles += o.handles; found = true; break; }
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
        ListView_SetItemText(g.listProcs, row, 1, const_cast<wchar_t*>(std::to_wstring(o.pid).c_str()));
        ListView_SetItemText(g.listProcs, row, 2,
                             const_cast<wchar_t*>((o.desc.empty() ? L"—" : o.desc).c_str()));
        ListView_SetItemText(g.listProcs, row, 3,
                             const_cast<wchar_t*>((std::wstring(L"×") + std::to_wstring(o.handles)).c_str()));
        const bool prot = o.prot;
        const std::wstring status = prot ? L"受保护" : (o.image ? L"映射" : L"占用");
        ListView_SetItemText(g.listProcs, row, 4, const_cast<wchar_t*>(status.c_str()));
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
        std::wstring text = L"另有 " + std::to_wstring(skipped) + L" 个受保护进程无法检查（可能是系统进程或安全软件）";
        LVITEMW lv{};
        lv.mask = LVIF_TEXT;
        lv.iItem = row;
        lv.pszText = const_cast<wchar_t*>(text.c_str());
        ListView_InsertItem(g.listProcs, &lv);
        ListView_SetItemText(g.listProcs, row, 4, const_cast<wchar_t*>(L"未知"));
    }
    SendMessageW(g.listProcs, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.listProcs, nullptr, TRUE);
}

void UpdateButtons() {
    const bool enable = !g.busy;
    for (HWND h : { g.btnAddFile, g.btnAddDir, g.btnUnlock, g.btnDelete, g.btnRename,
                    g.btnMove, g.btnCopy, g.btnRescan, g.btnDeep }) {
        if (h) EnableWindow(h, enable);
    }
    if (g.chkForce) EnableWindow(g.chkForce, !g.busy);
    if (g.btnDeep) EnableWindow(g.btnDeep, !g.busy);
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
                it.owners.push_back(std::move(uo));
            }
            PostScanItem(id, it, rep.state == OccupancyState::NotFound);
        }
        PostStatus(L"扫描完成");
    });
    g.scanThreads.push_back(std::move(t));
}

// 深扫：一次请求 + 结果收集器（进度只用于状态栏）
void StartDeepScan(const std::vector<std::pair<uint64_t, std::wstring>>& targets) {
    if (targets.empty()) return;
    PostStatus(L"正在请求管理员权限以深扫…");

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
                PostStatus(L"深扫中 " + std::to_wstring(static_cast<int>(v.GetNumber("cur", 0))) + L"/" +
                           std::to_wstring(static_cast<int>(v.GetNumber("total", 0))));
                return;
            }
            result = v;
            got = true;
        }, &err, 300000);

        if (!ok || !got) {
            PostStatus(err.empty() ? L"深扫失败" : err);
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
                        it.owners.push_back(std::move(uo));
                    }
                }
                PostScanItem(it.id, it, false);
            }
        }
        PostStatus(L"深扫完成（管理员）");
        PostMessageW(g.hwnd, WM_APP_SCAN_DONE, 0, 0);
    });
    g.scanThreads.push_back(std::move(t));
}

// ---------------------------------------------------------------- 加入列表
void AddPaths(const std::vector<std::wstring>& raw) {
    if (raw.empty()) return;
    if (raw.size() > 500) {
        std::wstring msg = L"一次拖入了 " + std::to_wstring(raw.size()) +
                           L" 个对象，超过单批 500 的上限。请分批处理，避免误删整棵目录。";
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
        if (rejected) SetStatus(L"没有可加入的路径（UNC / 回收站 / 无法解析的快捷方式已拒绝）");
        return;
    }

    for (auto& it : g.items) {
        for (const auto& kv : toScan) if (kv.first == it.id) it.scanning = true;
    }
    RefreshFileList();
    SetStatus(L"正在扫描占用进程…");
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
    SetStatus(L"重新扫描中…");
    StartLocalScan(targets);
}

void ApplyOpResult(const OpDone& done) {
    g.busy = false;
    UpdateButtons();
    SetStatus(done.ok ? L"操作完成" : (done.summary.empty() ? L"操作失败" : done.summary));

    // 用户点了 UAC 的「否」是正常路径：一行提示 + 重试入口，不当异常弹错误框
    const bool cancelled = done.summary.find(L"已取消提权") != std::wstring::npos;
    if (cancelled) {
        MessageBoxW(g.hwnd, L"已取消提权，无法执行操作。\n再次点击按钮可以重试。", L"Unlocker",
                    MB_ICONINFORMATION | MB_OK);
        return;
    }

    std::wstring text = done.summary;
    if (!done.detail.empty()) {
        text += L"\n\n";
        text += done.detail;
    }
    MessageBoxW(g.hwnd, text.c_str(), done.ok ? L"Unlocker" : L"Unlocker · 未完成",
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
                    denied ? L"选中的项目全部被内置黑名单阻止，无法操作。"
                           : L"没有可操作的项目，请先添加文件。",
                    L"Unlocker", MB_ICONINFORMATION | MB_OK);
        return;
    }

    g.busy = true;
    UpdateButtons();
    const bool force = g.forceOn;
    SetStatus(L"正在请求管理员权限…（首次操作会弹出 UAC）");

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
                PostStatus(L"处理中 " + std::to_wstring(static_cast<int>(v.GetNumber("cur", 0))) + L"/" +
                           std::to_wstring(static_cast<int>(v.GetNumber("total", 0))));
                return;
            }
            PostStatus(L"执行组件：运行中（管理员）");
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

        std::wstring summary = L"成功 " + std::to_wstring(static_cast<int>(result.GetNumber("okCount", 0))) +
                               L"，失败 " + std::to_wstring(static_cast<int>(result.GetNumber("failCount", 0))) +
                               L"，跳过 " + std::to_wstring(static_cast<int>(result.GetNumber("skipCount", 0)));
        if (result.GetBool("reboot", false)) summary += L"\n有项目已排队到重启后执行。";
        done->summary = summary;

        std::wstring detail;
        const json::Value* results = result.Find("results");
        if (results && results->type == json::Value::Type::Array) {
            for (const auto& it : results->arr) {
                const bool itemOk = it.GetBool("ok");
                const bool skipped = it.GetBool("skipped");
                if (itemOk && !skipped) continue;
                if (detail.size() > 4000) break;
                detail += (skipped ? L"[跳过] " : L"[失败] ") + Utf8ToUtf16(it.GetString("path")) + L"\n";
                const std::wstring rule = Utf8ToUtf16(it.GetString("rule"));
                if (!rule.empty()) detail += L"    规则 " + rule + L"\n";
                const std::wstring msg = Utf8ToUtf16(it.GetString("msg"));
                if (!msg.empty()) detail += L"    " + msg + L"\n";
            }
        }
        done->detail = detail;
        PostMessageW(g.hwnd, WM_APP_OP_DONE, 1, reinterpret_cast<LPARAM>(done));
    });
}

// ---------------------------------------------------------------- 布局
void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int W = rc.right, H = rc.bottom;

    const int pad = 8;
    const int rowH = 30;
    int y = pad;

    if (g.banner) {
        if (IsWindowVisible(g.banner)) {
            const int bh = 34;
            MoveWindow(g.banner, pad, y, W - pad * 2, bh, TRUE);
            y += bh + 6;
        }
    }

    int x = pad;
    const int bw = 96;
    MoveWindow(g.btnAddFile, x, y, bw, rowH, TRUE); x += bw + 6;
    MoveWindow(g.btnAddDir, x, y, bw, rowH, TRUE); x += bw + 6;
    MoveWindow(g.btnDeep, x, y, 132, rowH, TRUE); x += 132 + 6;
    MoveWindow(g.chkForce, x, y + 6, 110, rowH - 8, TRUE);

    y += rowH + 8;

    const int statusH = 26;
    const int bottomH = rowH + 10;
    int listH = H - y - statusH - bottomH;
    if (listH < 120) listH = 120;
    const int fileH = listH * 55 / 100;
    const int procH = listH - fileH - 6;

    MoveWindow(g.listFiles, pad, y, W - pad * 2, fileH, TRUE);
    MoveWindow(g.hint, pad, y, W - pad * 2, fileH, TRUE);
    y += fileH + 6;
    MoveWindow(g.listProcs, pad, y, W - pad * 2, procH, TRUE);
    y += procH + 6;

    int bx = pad;
    MoveWindow(g.btnUnlock, bx, y, 84, rowH, TRUE); bx += 90;
    MoveWindow(g.btnDelete, bx, y, 108, rowH, TRUE); bx += 114;
    MoveWindow(g.btnRename, bx, y, 108, rowH, TRUE); bx += 114;
    MoveWindow(g.btnMove, bx, y, 108, rowH, TRUE); bx += 114;
    MoveWindow(g.btnCopy, bx, y, 108, rowH, TRUE); bx += 114;
    MoveWindow(g.btnRescan, bx, y, 96, rowH, TRUE);

    MoveWindow(g.status, pad, H - statusH, W - pad * 2, statusH, TRUE);
}

void CreateControls(HWND hwnd) {
    const HINSTANCE inst = g.inst;
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                               hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    };

    g.btnAddFile = make(L"BUTTON", L"添加文件", BS_PUSHBUTTON, ID_ADD_FILE);
    g.btnAddDir  = make(L"BUTTON", L"添加文件夹", BS_PUSHBUTTON, ID_ADD_DIR);
    g.btnDeep    = make(L"BUTTON", L"以管理员身份深扫", BS_PUSHBUTTON, ID_DEEP_SCAN);
    g.chkForce   = make(L"BUTTON", L"强制模式", BS_AUTOCHECKBOX | BS_PUSHLIKE, ID_FORCE);

    g.listFiles = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                  0, 0, 10, 10, hwnd, nullptr, inst, nullptr);
    ListView_SetExtendedListViewStyle(g.listFiles, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

    struct Col { const wchar_t* name; int width; };
    const Col fileCols[] = { { L"路径", 430 }, { L"大小", 90 }, { L"占用者", 190 }, { L"状态", 130 } };
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<wchar_t*>(fileCols[i].name);
        c.cx = fileCols[i].width;
        c.iSubItem = i;
        ListView_InsertColumn(g.listFiles, i, &c);
    }

    g.listProcs = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                  0, 0, 10, 10, hwnd, nullptr, inst, nullptr);
    ListView_SetExtendedListViewStyle(g.listProcs, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const Col procCols[] = { { L"进程", 220 }, { L"PID", 70 }, { L"描述", 260 }, { L"句柄", 60 }, { L"状态", 90 } };
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<wchar_t*>(procCols[i].name);
        c.cx = procCols[i].width;
        c.iSubItem = i;
        ListView_InsertColumn(g.listProcs, i, &c);
    }

    g.hint = CreateWindowExW(0, L"STATIC", L"把文件或文件夹拖到这里\n（也可以点击上方「添加文件 / 添加文件夹」）",
                             WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                             0, 0, 10, 10, hwnd, nullptr, inst, nullptr);
    SendMessageW(g.hint, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);

    g.btnUnlock = make(L"BUTTON", L"解锁", BS_PUSHBUTTON, ID_UNLOCK);
    g.btnDelete = make(L"BUTTON", L"解锁并删除", BS_PUSHBUTTON, ID_DELETE);
    g.btnRename = make(L"BUTTON", L"解锁并改名", BS_PUSHBUTTON, ID_RENAME);
    g.btnMove   = make(L"BUTTON", L"解锁并移动", BS_PUSHBUTTON, ID_MOVE);
    g.btnCopy   = make(L"BUTTON", L"解锁并复制", BS_PUSHBUTTON, ID_COPY);
    g.btnRescan = make(L"BUTTON", L"重新扫描", BS_PUSHBUTTON, ID_RESCAN);

    g.status = CreateWindowExW(0, L"STATIC", L"执行组件：未启动",
                               WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                               0, 0, 10, 10, hwnd, nullptr, inst, nullptr);
    SendMessageW(g.status, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);

    if (g.elevated) {
        g.banner = CreateWindowExW(0, L"STATIC",
                                   L"当前以管理员权限运行。推荐以普通权限启动，拖放与右键菜单会更稳定。",
                                   WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX,
                                   0, 0, 10, 10, hwnd, nullptr, inst, nullptr);
        SendMessageW(g.banner, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    }

    DragAcceptFiles(hwnd, TRUE);   // 方案 B 的收益：中完整性窗口与资源管理器同级，UIPI 完全不拦
    StartUiPipeListener(hwnd);
}

// ---------------------------------------------------------------- 对话框
void ShowDeniedDialog(const UiItem& it) {
    std::wstring text = it.rule.empty() ? L"已被内置黑名单阻止" : (it.rule + L" · 已阻止");
    text += L"\n";
    text += it.canonical.empty() ? it.path : it.canonical;
    text += L"\n\n";
    text += it.reason;
    text += L"\n此规则为内置保护，无法关闭。";
    MessageBoxW(g.hwnd, text.c_str(), L"Unlocker", MB_ICONWARNING | MB_OK);
}

// ---------------------------------------------------------------- 窗口过程
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g.hwnd = hwnd;
            g.elevated = IsUiElevated();
            if (g.elevated) PatchDropForElevatedUi(hwnd);
            CreateControls(hwnd);
            Layout(hwnd);
            UpdateButtons();
            SetStatus(g.failClosed ? L"规则表自检失败：已进入保护模式，全部操作被拒绝"
                                   : (g.elevated ? L"执行组件：运行中（管理员权限 UI）"
                                                 : L"执行组件：未启动（首次写操作时会请求管理员权限）"));
            return 0;

        case WM_SIZE:
            Layout(hwnd);
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 760;
            mmi->ptMinTrackSize.y = 480;
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

            if (hdr->idFrom == reinterpret_cast<UINT_PTR>(g.listFiles) && hdr->code == LVN_ITEMCHANGED) {
                RefreshProcessList();
                return 0;
            }
            if (hdr->idFrom == reinterpret_cast<UINT_PTR>(g.listFiles) && hdr->code == NM_CUSTOMDRAW) {
                auto* cd = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    const size_t idx = static_cast<size_t>(cd->nmcd.dwItemSpec);
                    if (idx < g.items.size()) {
                        const auto& it = g.items[idx];
                        cd->clrText = StateColor(it.state, it.verdict == Verdict::Deny);
                        if (it.verdict == Verdict::Deny) cd->clrTextBk = RGB(255, 238, 238);
                    }
                    return CDRF_NEWFONT;
                }
                return CDRF_DODEFAULT;
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
                        MessageBoxW(hwnd, L"没有可扫描的项目。", L"Unlocker", MB_ICONINFORMATION | MB_OK);
                        return 0;
                    }
                    g.busy = true;
                    UpdateButtons();
                    StartDeepScan(targets);
                    return 0;
                }
                case ID_FORCE:
                    g.forceOn = SendMessageW(g.chkForce, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
                default:
                    return 0;
            }
        }

        case WM_CLOSE: {
            if (g.busy) {
                if (MessageBoxW(hwnd, L"操作正在进行中，确定要退出吗？", L"Unlocker",
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
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
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"UnlockerMainWindow";
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    RECT want{ 0, 0, 980, 640 };
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, FALSE, 0);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Unlocker",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                want.right - want.left, want.bottom - want.top,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    if (!initialPaths.empty()) AddPaths(initialPaths);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
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
    CoUninitialize();
    return 0;
}

}  // namespace ul
