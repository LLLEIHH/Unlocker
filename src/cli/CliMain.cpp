#include "cli/CliMain.h"

#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <vector>

#include "common/ErrorMap.h"
#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/FileOps.h"
#include "core/HandleScanner.h"
#include "core/PathResolver.h"
#include "core/PolicyGuard.h"
#include "core/Privilege.h"
#include "ipc/Json.h"
#include "ipc/PipeClient.h"
#include "ipc/Protocol.h"

namespace ul {
namespace {

// ---------------------------------------------------------------- 控制台输出
// 主程序是 GUI 子系统：先 AttachConsole 借用父进程控制台，拿不到就退回重定向句柄。
void EnsureConsole() {
    // 已经重定向到管道/文件时不要再抢 CONOUT$，否则脚本拿不到输出（--json 会静默丢数据）
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) {
        const DWORD type = GetFileType(out);
        if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK) return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$", "r", stdin);
    }
}

void Out(const std::wstring& s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        DWORD w = 0;
        WriteConsoleW(h, s.c_str(), static_cast<DWORD>(s.size()), &w, nullptr);
    } else {
        const std::string u8 = Utf16ToUtf8(s);
        DWORD w = 0;
        WriteFile(h, u8.data(), static_cast<DWORD>(u8.size()), &w, nullptr);
    }
}

// 已经是 UTF-8 的载荷（例如 JSON）直接原样写出
void Out(const std::string& s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h) return;
    DWORD w = 0;
    WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &w, nullptr);
}

constexpr wchar_t kUsage[] =
    L"Unlocker v0.8\n"
    L"用法: Unlocker.exe [选项] <路径...>\n\n"
    L"  /u            仅解锁（关句柄），不执行文件操作\n"
    L"  /d            解锁并删除\n"
    L"  /r <新名>     解锁并重命名\n"
    L"  /m <目标目录> 解锁并移动\n"
    L"  /c <目标目录> 解锁并复制\n"
    L"  --force       强制模式（允许终止进程；不能覆盖黑名单）\n"
    L"  --reboot      失败时排队到重启\n"
    L"  --list        只列出占用者，不做任何修改\n"
    L"  --json        机器可读输出\n"
    L"  --quiet       静默（且禁止任何 UAC 弹窗）\n"
    L"  --install     安装资源管理器右键菜单（需管理员）\n"
    L"  --uninstall   卸载右键菜单（需管理员）\n\n"
    L"退出码: 0 成功 / 1 参数错误 / 2 未找到占用者 / 3 权限不足 / 4 被黑名单拦截\n"
    L"        5 解锁失败 / 6 需要重启 / 7 需要确认但 quiet / 8 需要提权但 quiet\n"
    L"不带动作参数时打开图形界面并载入这些路径。\n";

bool IsFlag(const std::wstring& a, const wchar_t* name) { return IEquals(a, name); }

std::wstring JsonString(const std::wstring& s) { return s; }

void PrintItemHuman(const ItemResult& r) {
    std::wstring line = L"  ";
    line += r.skipped ? L"[跳过] " : (r.ok ? L"[成功] " : L"[失败] ");
    line += r.path;
    if (!r.rule.empty()) line += L"  规则=" + r.rule;
    if (r.level >= 0) line += L"  层级=L" + std::to_wstring(r.level);
    if (!r.message.empty()) line += L"\n         " + r.message;
    line += L"\n";
    Out(line);
}

int CliExitFromBatch(const BatchResult& b) {
    if (b.failCount == 0 && b.skipCount == 0) return b.anyReboot ? kExitRebootRequired : kExitOk;
    for (const auto& it : b.items) {
        if (it.code == kExitPolicyDenied) return kExitPolicyDenied;
        if (it.code == kExitNeedConfirm) return kExitNeedConfirm;
    }
    if (b.skipCount > 0) return kExitNeedConfirm;
    return kExitUnlockFailed;
}

// ---------------------------------------------------------------- 右键菜单注册
constexpr wchar_t kClsid[] = L"{6E2C4A5B-1B3C-4F62-9C0E-2D8E3A9B7F41}";

bool WriteReg(const std::wstring& sub, const std::wstring& name, const std::wstring& value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const bool ok = RegSetValueExW(k, name.empty() ? nullptr : name.c_str(), 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

bool DeleteRegTree(const std::wstring& sub) {
    return RegDeleteTreeW(HKEY_CLASSES_ROOT, sub.c_str()) == ERROR_SUCCESS;
}

int InstallShell(bool install) {
    if (!IsElevated()) {
        Out(L"需要管理员权限才能修改右键菜单，请用管理员终端运行。\n");
        return kExitAccessDenied;
    }

    if (!install) {
        DeleteRegTree(std::wstring(L"CLSID\\") + kClsid);
        DeleteRegTree(L"*\\shellex\\ContextMenuHandlers\\Unlocker");
        DeleteRegTree(L"Directory\\shellex\\ContextMenuHandlers\\Unlocker");
        Out(L"右键菜单已卸载。资源管理器会在下次启动时生效。\n");
        return kExitOk;
    }

    const std::wstring dll = PathResolver::SelfDir() + L"\\UnlockerShell.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Out(L"找不到 UnlockerShell.dll，请把它和 Unlocker.exe 放在同一目录。\n");
        return kExitBadArgs;
    }

    const std::wstring clsidKey = std::wstring(L"CLSID\\") + kClsid;
    bool ok = true;
    ok &= WriteReg(clsidKey, L"", L"Unlocker Shell Extension");
    ok &= WriteReg(clsidKey + L"\\InprocServer32", L"", dll);
    ok &= WriteReg(clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    ok &= WriteReg(L"*\\shellex\\ContextMenuHandlers\\Unlocker", L"", kClsid);
    ok &= WriteReg(L"Directory\\shellex\\ContextMenuHandlers\\Unlocker", L"", kClsid);

    if (!ok) {
        Out(L"写入注册表失败（可能被安全软件拦截）。\n");
        return kExitAccessDenied;
    }
    Out(L"右键菜单已安装。Win11 新菜单里请在「显示更多选项」中查看。\n");
    return kExitOk;
}

// ---------------------------------------------------------------- L1 扫描输出
int RunList(const std::vector<std::wstring>& paths, bool json, bool quiet) {
    if (!paths.empty()) {
        const bool elevated = IsElevated();
        if (!json) {
            Out(L"目标             状态        大小        占用者\n");
            Out(L"--------------------------------------------------------------\n");
        }

        json::Value arr = json::Value::MakeArray();
        bool anyIncomplete = false;

        for (const auto& p : paths) {
            FileReport rep = HandleScanner::Scan(p, ScanOptions{ elevated, true });
            PolicyResult pol = PolicyGuard::Evaluate(p);
            if (rep.needElevation) anyIncomplete = true;

            json::Value o = proto::ReportToJson(rep);
            o.Set("verdict", json::Value::MakeNumber(static_cast<int>(pol.verdict)));
            o.Set("rule", json::Value::MakeString(Utf16ToUtf8(PolicyGuard::RuleName(pol.ruleId))));
            arr.arr.push_back(std::move(o));

            if (!json) {
                std::wstring owners;
                std::vector<DWORD> seen;
                for (const auto& ow : rep.owners) {
                    bool dup = false;
                    for (DWORD d : seen) if (d == ow.proc.pid) { dup = true; break; }
                    if (dup) continue;
                    seen.push_back(ow.proc.pid);
                    if (!owners.empty()) owners += L", ";
                    owners += ow.proc.name + L" (" + std::to_wstring(ow.proc.pid) + L")";
                    if (ow.proc.handleCount > 1) owners += L" ×" + std::to_wstring(ow.proc.handleCount);
                }
                if (owners.empty()) owners = L"—";

                const bool inconclusive = rep.state == OccupancyState::Locked ||
                                          rep.state == OccupancyState::LockedByKernel ||
                                          rep.state == OccupancyState::AccessDenied;
                std::wstring line = rep.canonical + L"\n    " + OccupancyText(rep.state) + L"   " +
                                    FormatSize(rep.size) + L"   " + owners + L"\n";
                if (pol.Denied()) {
                    line += L"    [内置黑名单] " + PolicyGuard::RuleName(pol.ruleId) + L"：" + pol.reason + L"\n";
                } else if (pol.NeedsConfirm()) {
                    line += L"    [需确认] " + PolicyGuard::RuleName(pol.ruleId) + L"：" + pol.reason + L"\n";
                }
                // 只有在结论可能被「看不到的进程」影响时才提这一行，否则是纯噪音
                if (inconclusive && !rep.skippedPids.empty()) {
                    line += L"    另有 " + std::to_wstring(rep.skippedPids.size()) +
                            L" 个受保护进程无法检查（可能是系统进程或安全软件）\n";
                }
                if (rep.needElevation && !elevated) {
                    line += L"    需要管理员权限才能完整检查：以管理员身份重新运行，或使用 /u /d 等写操作触发提权\n";
                }
                Out(line);
            }
            if (!json && rep.error &&
                (rep.state == OccupancyState::Error || rep.state == OccupancyState::AccessDenied)) {
                Out(L"    错误: " + ErrorWithCode(rep.error) + L"\n");
            }
        }

        if (json) {
            json::Value root = json::Value::MakeObject();
            root.Set("ok", json::Value::MakeBool(true));
            root.Set("incomplete", json::Value::MakeBool(anyIncomplete));
            root.Set("elevated", json::Value::MakeBool(elevated));
            root.Set("items", std::move(arr));
            Out((json::Dump(root)));
            Out(L"\n");
        } else {
            Out(L"\n");
            if (anyIncomplete && !quiet) {
                Out(L"提示：有进程需要管理员权限才能检查。以管理员身份运行可看到完整结果。\n");
            }
        }
    }
    return kExitOk;
}

bool BuildRequestFromArgs(IpcOp op, const std::vector<std::wstring>& paths, const std::wstring& arg,
                          bool force, bool reboot, bool quiet, IpcRequest* req) {
    req->op = op;
    req->items = paths;
    req->arg = arg;
    req->force = force;
    req->reboot = reboot;
    req->quiet = quiet;
    req->caller = "CLI";
    return true;
}

int RunViaWorker(const IpcRequest& req, bool json) {
    PipeClient client;
    std::wstring err;
    if (!client.EnsureStarted(&err)) {
        Out(L"无法启动执行组件：" + err + L"\n");
        return err.find(L"已取消提权") != std::wstring::npos ? kExitAccessDenied : kExitUnlockFailed;
    }

    json::Value finalResult;
    bool got = false;
    const bool ok = client.Request(req, [&](const json::Value& v) {
        if (v.GetString("evt") == "progress") {
            if (!json) {
                Out(L"\r  处理中 " + std::to_wstring(static_cast<int>(v.GetNumber("cur", 0))) + L"/" +
                    std::to_wstring(static_cast<int>(v.GetNumber("total", 0))) + L"   ");
            }
            return;
        }
        finalResult = v;
        got = true;
    }, &err, 300000);

    if (!json && got) Out(L"\n");
    if (!ok || !got) {
        Out(L"执行失败：" + err + L"\n");
        return kExitUnlockFailed;
    }

    if (json) {
        Out((json::Dump(finalResult)));
        Out(L"\n");
    } else {
        const json::Value* results = finalResult.Find("results");
        if (results && results->type == json::Value::Type::Array) {
            for (const auto& it : results->arr) {
                ItemResult r;
                r.path = Utf8ToUtf16(it.GetString("path"));
                r.ok = it.GetBool("ok");
                r.skipped = it.GetBool("skipped");
                r.code = static_cast<DWORD>(it.GetNumber("code", 0));
                r.level = static_cast<int>(it.GetNumber("level", -1));
                r.rule = Utf8ToUtf16(it.GetString("rule"));
                r.message = Utf8ToUtf16(it.GetString("msg"));
                PrintItemHuman(r);
            }
        }
        Out(L"汇总：成功 " + std::to_wstring(static_cast<int>(finalResult.GetNumber("okCount", 0))) +
            L"，失败 " + std::to_wstring(static_cast<int>(finalResult.GetNumber("failCount", 0))) +
            L"，跳过 " + std::to_wstring(static_cast<int>(finalResult.GetNumber("skipCount", 0))) + L"\n");
    }
    return static_cast<int>(finalResult.GetNumber("code", kExitOk));
}

int RunLocally(IpcOp op, const std::vector<std::wstring>& paths, const std::wstring& arg,
               bool force, bool reboot, bool quiet, bool json) {
    ApplyOptions opts;
    opts.force = force;
    opts.allowReboot = reboot;
    opts.quiet = quiet;
    opts.elevated = IsElevated();
    opts.caller = L"CLI";

    ConfirmFn confirm;
    if (!quiet) {
        confirm = [](const ConfirmRequest& cr) -> bool {
            std::wstring text = L"需要确认：" + cr.rule + L"\n" + cr.reason + L"\n\n";
            text += L"将影响 " + std::to_wstring(cr.paths.size()) + L" 个对象。继续吗？";
            return MessageBoxW(nullptr, text.c_str(), L"Unlocker 确认",
                               MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_DEFBUTTON2) == IDYES;
        };
    }

    BatchResult batch;
    switch (op) {
        case IpcOp::Unlock: batch = FileOps::Unlock(paths, opts, confirm); break;
        case IpcOp::Delete: batch = FileOps::Delete(paths, opts, confirm); break;
        case IpcOp::Rename: batch = FileOps::Rename(paths.front(), arg, opts, confirm); break;
        case IpcOp::Move:   batch = FileOps::Move(paths, arg, opts, confirm); break;
        case IpcOp::Copy:   batch = FileOps::Copy(paths, arg, opts, confirm); break;
        default:            return kExitBadArgs;
    }

    if (json) {
        json::Value root = json::Value::MakeObject();
        root.Set("ok", json::Value::MakeBool(batch.failCount == 0 && batch.skipCount == 0));
        root.Set("code", json::Value::MakeNumber(CliExitFromBatch(batch)));
        json::Value arr = json::Value::MakeArray();
        for (const auto& it : batch.items) arr.arr.push_back(proto::ItemResultToJson(it));
        root.Set("results", std::move(arr));
        Out((json::Dump(root)));
        Out(L"\n");
    } else {
        for (const auto& it : batch.items) PrintItemHuman(it);
        Out(L"汇总：成功 " + std::to_wstring(batch.okCount) + L"，失败 " +
            std::to_wstring(batch.failCount) + L"，跳过 " + std::to_wstring(batch.skipCount) + L"\n");
    }
    return CliExitFromBatch(batch);
}

int RunScanViaWorker(const std::vector<std::wstring>& paths, bool json) {
    PipeClient client;
    std::wstring err;
    if (!client.EnsureStarted(&err)) {
        Out(L"无法启动执行组件：" + err + L"\n");
        return kExitAccessDenied;
    }
    IpcRequest req;
    req.op = IpcOp::Scan;
    req.items = paths;
    req.caller = "CLI";
    json::Value finalResult;
    bool got = false;
    if (!client.Request(req, [&](const json::Value& v) {
            if (v.GetString("evt") == "progress") {
                if (!json) Out(L"\r  扫描中 " + std::to_wstring(static_cast<int>(v.GetNumber("cur", 0))) + L"   ");
                return;
            }
            finalResult = v;
            got = true;
        }, &err, 300000) || !got) {
        Out(L"深扫失败：" + err + L"\n");
        return kExitUnlockFailed;
    }
    if (!json) Out(L"\n");

    if (json) {
        Out((json::Dump(finalResult)));
        Out(L"\n");
    } else {
        const json::Value* results = finalResult.Find("results");
        if (results && results->type == json::Value::Type::Array) {
            for (const auto& it : results->arr) {
                std::wstring line = Utf8ToUtf16(it.GetString("path")) + L"\n    " +
                                    Utf8ToUtf16(it.GetString("stateText")) + L"\n";
                const json::Value* owners = it.Find("owners");
                if (owners && owners->type == json::Value::Type::Array) {
                    for (const auto& ow : owners->arr) {
                        line += L"    " + Utf8ToUtf16(ow.GetString("name")) + L" (PID " +
                                std::to_wstring(static_cast<int>(ow.GetNumber("pid", 0))) + L") ×" +
                                std::to_wstring(static_cast<int>(ow.GetNumber("handles", 1)));
                        if (!ow.GetString("desc").empty()) line += L"  " + Utf8ToUtf16(ow.GetString("desc"));
                        line += L"\n";
                    }
                }
                const json::Value* skipped = it.Find("skippedPids");
                if (skipped && !skipped->arr.empty()) {
                    line += L"    另有 " + std::to_wstring(skipped->arr.size()) + L" 个受保护进程无法检查\n";
                }
                Out(line);
            }
        }
    }
    return kExitOk;
}

}  // namespace

bool CliHasActionFlag(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (IsFlag(a, L"/u") || IsFlag(a, L"/d") || IsFlag(a, L"/r") || IsFlag(a, L"/m") ||
            IsFlag(a, L"/c") || IsFlag(a, L"--force") || IsFlag(a, L"--reboot") ||
            IsFlag(a, L"--list") || IsFlag(a, L"--json") || IsFlag(a, L"--quiet") ||
            IsFlag(a, L"--help") || IsFlag(a, L"/?") || IsFlag(a, L"--install") ||
            IsFlag(a, L"--uninstall") || IsFlag(a, L"--version")) {
            return true;
        }
    }
    return false;
}

int CliMain(int argc, wchar_t** argv) {
    EnsureConsole();
    LogInit(L"Unlocker");

    IpcOp op = IpcOp::Invalid;
    std::wstring arg;
    std::vector<std::wstring> paths;
    bool force = false, reboot = false, list = false, json = false, quiet = false;
    bool install = false, uninstall = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (IsFlag(a, L"/u")) { op = IpcOp::Unlock; }
        else if (IsFlag(a, L"/d")) { op = IpcOp::Delete; }
        else if (IsFlag(a, L"/r")) { op = IpcOp::Rename; if (i + 1 < argc) arg = argv[++i]; }
        else if (IsFlag(a, L"/m")) { op = IpcOp::Move; if (i + 1 < argc) arg = argv[++i]; }
        else if (IsFlag(a, L"/c")) { op = IpcOp::Copy; if (i + 1 < argc) arg = argv[++i]; }
        else if (IsFlag(a, L"--force")) { force = true; }
        else if (IsFlag(a, L"--reboot")) { reboot = true; }
        else if (IsFlag(a, L"--list")) { list = true; }
        else if (IsFlag(a, L"--json")) { json = true; }
        else if (IsFlag(a, L"--quiet")) { quiet = true; }
        else if (IsFlag(a, L"--install")) { install = true; }
        else if (IsFlag(a, L"--uninstall")) { uninstall = true; }
        else if (IsFlag(a, L"--help") || IsFlag(a, L"/?")) { Out(kUsage); return kExitOk; }
        else if (IsFlag(a, L"--version")) { Out(L"Unlocker v0.8 (x64, protocol v3)\n"); return kExitOk; }
        else paths.push_back(a);
    }

    if (install || uninstall) return InstallShell(install);

    if (list && op == IpcOp::Invalid) {
        if (paths.empty()) { Out(kUsage); return kExitBadArgs; }
        return RunList(paths, json, quiet);
    }
    if (op == IpcOp::Invalid) {
        Out(kUsage);
        return kExitBadArgs;
    }
    if (paths.empty()) {
        Out(L"缺少路径参数。\n");
        return kExitBadArgs;
    }
    if ((op == IpcOp::Rename || op == IpcOp::Move || op == IpcOp::Copy) && arg.empty()) {
        Out(L"缺少目标参数（/r 新名、/m 目标目录、/c 目标目录）。\n");
        return kExitBadArgs;
    }

    // 初始化与前置裁决
    PathResolver::Init();
    std::wstring perr;
    if (!PolicyGuard::Init(&perr) || PolicyGuard::FailClosed()) {
        Out(L"内置规则表自检失败，已进入保护模式，所有操作拒绝。\n");
        return kExitPolicyDenied;
    }
    HandleScanner::Init(nullptr);

    PolicyResult batchPolicy = PolicyGuard::EvaluateBatch(paths);
    if (batchPolicy.Denied()) {
        Out(PolicyGuard::ThreePartMessage(batchPolicy) + L"\n");
        LogAudit(L"DENY", PolicyGuard::RuleName(batchPolicy.ruleId), paths.front(),
                 batchPolicy.canonical, Utf8ToUtf16(OpName(op)), L"CLI");
        return kExitPolicyDenied;   // --force 也不能覆盖
    }

    const bool elevated = IsElevated();

    if (!elevated && quiet) {
        // --quiet 禁止任何形式的 UAC：否则脚本会卡在一个没人点的弹窗上
        Out(L"需要管理员权限，但 --quiet 下不允许弹出 UAC。请用管理员终端运行。\n");
        return kExitNeedElevation;
    }

    if (list) {
        // --list 只读：先 L1，遇到打不开的进程不自动提权，输出 incomplete
        return RunList(paths, json, quiet);
    }

    if (elevated) return RunLocally(op, paths, arg, force, reboot, quiet, json);

    IpcRequest req;
    BuildRequestFromArgs(op, paths, arg, force, reboot, quiet, &req);
    return RunViaWorker(req, json);
}

}  // namespace ul
