#include "ipc/PipeServer.h"

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <vector>

#include "common/ErrorMap.h"
#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/FileOps.h"
#include "core/HandleScanner.h"
#include "core/PathResolver.h"
#include "core/PolicyGuard.h"
#include "core/Privilege.h"
#include "core/RebootScheduler.h"

namespace ul {
namespace {

constexpr DWORD kIdleTimeoutMs = 5 * 60 * 1000;   // 空闲 5 分钟自退
constexpr DWORD kHelloTimeoutMs = 15000;

bool      g_testMode = false;
std::atomic<bool> g_cancel{false};

bool IsValidPipeName(const std::wstring& name) {
    // 不允许裸跑 worker：管道名必须是本程序生成的形态，否则 Unlocker.exe --worker 就是个 LOLBin
    if (!IStartsWith(name, L"\\\\?\\pipe\\") && !IStartsWith(name, L"\\\\.\\pipe\\")) return false;
    const std::wstring lower = ToLower(name);
    const std::wstring prefix = L"unlocker.w.";
    const size_t pos = lower.find(prefix);
    if (pos == std::wstring::npos) return false;
    const std::wstring token = lower.substr(pos + prefix.size());
    if (token.size() < 32 || token.size() > 64) return false;
    for (wchar_t c : token) {
        const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
        if (!hex) return false;
    }
    return true;
}

HANDLE CreateServerPipe(const std::wstring& name, const std::wstring& sddl, DWORD* err) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        if (err) *err = GetLastError();
        return INVALID_HANDLE_VALUE;
    }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };

    HANDLE pipe = CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS | PIPE_WAIT,
        1, 64 * 1024, 64 * 1024, 0, &sa);
    if (err) *err = (pipe == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
    LocalFree(sd);
    return pipe;
}

bool ConnectServer(HANDLE pipe, DWORD timeoutMs) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    BOOL ok = ConnectNamedPipe(pipe, &ov);
    DWORD e = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && e == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &ov);
            CloseHandle(ov.hEvent);
            SetLastError(ERROR_TIMEOUT);
            return false;
        }
        DWORD got = 0;
        ok = GetOverlappedResult(pipe, &ov, &got, FALSE);
        e = ok ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(ov.hEvent);
    // ERROR_PIPE_CONNECTED 表示客户端已在等待，属于成功
    return ok || e == ERROR_PIPE_CONNECTED;
}

// 三重校验，缺一不可（4.10.3(2)）
bool VerifyClient(HANDLE pipe, const WorkerContext& ctx, unsigned long long workerStart,
                  std::wstring* why) {
    if (g_testMode) return true;

    ULONG clientPid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &clientPid)) {
        if (why) *why = L"无法获取客户端 PID";
        return false;
    }

    // a. PID 必须等于启动参数里的 --parent-pid
    if (clientPid != ctx.expectedUiPid) {
        if (why) *why = L"客户端 PID 与 --parent-pid 不一致";
        return false;
    }

    // b. 该 PID 的完整映像路径必须等于本程序的 exe 路径
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, clientPid);
    if (!h) {
        if (why) *why = L"无法打开客户端进程";
        return false;
    }
    wchar_t buf[32768]{};
    DWORD len = static_cast<DWORD>(std::size(buf));
    std::wstring clientImage;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) clientImage.assign(buf, len);
    CloseHandle(h);

    const std::wstring selfImage = PathResolver::SelfExePath();
    if (clientImage.empty() || !IEquals(PathResolver::Normalize(clientImage),
                                        PathResolver::Normalize(selfImage))) {
        if (why) *why = L"客户端映像路径不是本程序";
        return false;
    }

    // c. 客户端创建时间必须早于 worker 自己，防 PID 复用
    unsigned long long clientStart = 0;
    if (GetProcessCreationTime(clientPid, &clientStart) && clientStart >= workerStart) {
        if (why) *why = L"客户端创建时间晚于 worker（疑似 PID 复用）";
        return false;
    }
    return true;
}

std::wstring BuildSddl(const WorkerContext& ctx, std::wstring* err) {
    std::wstring uiSid = UserSidStringOfProcess(ctx.expectedUiPid);
    if (uiSid.empty()) uiSid = ctx.uiUserSid;   // 兜底：命令行传的 SID（worker 仍以父进程令牌为准）

    std::wstring selfSid = CurrentUserSidString();
    if (uiSid.empty() || selfSid.empty()) {
        if (err) *err = L"无法读取用户 SID（UI 或 worker 令牌不可用）";
        return {};
    }

    // 跨用户提权（标准用户输入管理员凭据）时 worker 跑在管理员账户，
    // 只放行自己会让标准用户的 UI 连不上 —— 必须同时放行 UI 侧 SID。
    std::wstring sddl = L"D:(A;;GA;;;" + uiSid + L")";
    if (!IEquals(uiSid, selfSid)) sddl += L"(A;;GA;;;" + selfSid + L")";
    // SACL：把对象标签压到 Medium。少了这条，中完整性 UI 连不上高完整性 worker，
    // 现象和「SDDL 写错」一模一样（ACCESS_DENIED），极容易查错方向。
    sddl += L"S:(ML;;NW;;;ME)";
    return sddl;
}

bool ShowConfirmDialog(const ConfirmRequest& req) {
    std::wstring text;
    text += L"此操作需要管理员权限的再次确认。\r\n\r\n";
    text += L"命中规则：" + req.rule + L"\r\n";
    text += L"原因：" + req.reason + L"\r\n\r\n";
    text += L"将影响 " + std::to_wstring(req.paths.size()) + L" 个对象：\r\n";
    const size_t shown = std::min<size_t>(req.paths.size(), 8);
    for (size_t i = 0; i < shown; ++i) text += L"  " + req.paths[i] + L"\r\n";
    if (req.paths.size() > shown) {
        text += L"  …以及另外 " + std::to_wstring(req.paths.size() - shown) + L" 个\r\n";
    }
    text += L"\r\n确认继续吗？";

    const int r = MessageBoxW(nullptr, text.c_str(), L"Unlocker · 需要确认",
                              MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND | MB_DEFBUTTON2);
    return r == IDYES;
}

int MapExitCode(const BatchResult& batch) {
    if (batch.failCount == 0 && batch.skipCount == 0) {
        return batch.anyReboot ? kExitRebootRequired : kExitOk;
    }
    for (const auto& it : batch.items) {
        if (it.code == kExitPolicyDenied) return kExitPolicyDenied;
        if (it.code == kExitNeedConfirm) return kExitNeedConfirm;
    }
    if (batch.okCount == 0 && batch.items.empty()) return kExitNoOwner;
    if (batch.skipCount > 0) return kExitNeedConfirm;
    return kExitUnlockFailed;
}

json::Value BatchToJson(uint64_t id, const BatchResult& batch, int exitCode) {
    json::Value root = json::Value::MakeObject();
    root.Set("id", json::Value::MakeNumber(static_cast<double>(id)));
    root.Set("ok", json::Value::MakeBool(batch.failCount == 0 && batch.skipCount == 0));
    root.Set("code", json::Value::MakeNumber(exitCode));
    root.Set("okCount", json::Value::MakeNumber(batch.okCount));
    root.Set("failCount", json::Value::MakeNumber(batch.failCount));
    root.Set("skipCount", json::Value::MakeNumber(batch.skipCount));
    root.Set("needConfirm", json::Value::MakeBool(batch.needConfirm));
    root.Set("reboot", json::Value::MakeBool(batch.anyReboot));

    json::Value results = json::Value::MakeArray();
    for (const auto& it : batch.items) results.arr.push_back(proto::ItemResultToJson(it));
    root.Set("results", std::move(results));
    return root;
}

void HandleScan(HANDLE pipe, const IpcRequest& req, const WorkerContext& ctx) {
    json::Value results = json::Value::MakeArray();
    const int total = static_cast<int>(req.items.size());
    int cur = 0;

    for (const auto& raw : req.items) {
        if (g_cancel.load()) break;
        ++cur;

        WriteFrame(pipe, json::Dump(proto::ProgressEvent(req.id, cur, total, L"扫描占用进程")));

        // worker 端重新归一化（UI 传来的路径不可信）
        FileReport rep = HandleScanner::Scan(raw, ScanOptions{ true, true }, nullptr, &g_cancel);

        PolicyResult pol = PolicyGuard::Evaluate(raw);
        json::Value item = proto::ReportToJson(rep);
        item.Set("verdict", json::Value::MakeNumber(static_cast<int>(pol.verdict)));
        item.Set("rule", json::Value::MakeString(Utf16ToUtf8(PolicyGuard::RuleName(pol.ruleId))));
        item.Set("reason", json::Value::MakeString(Utf16ToUtf8(pol.reason)));
        item.Set("canonical", json::Value::MakeString(Utf16ToUtf8(pol.canonical)));
        results.arr.push_back(std::move(item));
    }

    json::Value root = json::Value::MakeObject();
    root.Set("id", json::Value::MakeNumber(static_cast<double>(req.id)));
    root.Set("ok", json::Value::MakeBool(true));
    root.Set("evt", json::Value::MakeString("scan"));
    root.Set("results", std::move(results));
    root.Set("quiet", json::Value::MakeBool(ctx.quiet));
    WriteFrame(pipe, json::Dump(root));
}

void HandleFileOp(HANDLE pipe, const IpcRequest& req, const WorkerContext& ctx) {
    ApplyOptions opts;
    opts.force       = req.force;
    opts.allowReboot = req.reboot || req.op == IpcOp::Reboot;
    opts.quiet       = ctx.quiet || req.quiet;
    opts.elevated    = IsElevated();
    opts.caller      = L"worker";

    ConfirmFn confirm;
    if (!opts.quiet) {
        confirm = [](const ConfirmRequest& cr) -> bool { return ShowConfirmDialog(cr); };
    }

    // Confirm 的决定权只有一处：worker。UI 只标注「需确认」，不收集决定。
    BatchResult batch;
    switch (req.op) {
        case IpcOp::Scan:
            break;
        case IpcOp::Unlock:
            batch = FileOps::Unlock(req.items, opts, confirm);
            break;
        case IpcOp::Delete:
        case IpcOp::Reboot:
            batch = FileOps::Delete(req.items, opts, confirm);
            break;
        case IpcOp::Rename: {
            if (req.items.empty()) break;
            batch = FileOps::Rename(req.items.front(), req.arg, opts, confirm);
            break;
        }
        case IpcOp::Move:
            batch = FileOps::Move(req.items, req.arg, opts, confirm);
            break;
        case IpcOp::Copy:
            batch = FileOps::Copy(req.items, req.arg, opts, confirm);
            break;
        default:
            break;
    }

    const int code = MapExitCode(batch);
    WriteFrame(pipe, json::Dump(BatchToJson(req.id, batch, code)));

    for (const auto& it : batch.items) {
        if (!it.ok && !it.skipped) {
            LogE(L"操作失败 " + it.path + L"：" + it.message);
        }
    }
}

}  // namespace

void PipeServer::SetTestMode(bool on) { g_testMode = on; }

int PipeServer::Run(const WorkerContext& ctx) {
    if (!IsValidPipeName(ctx.pipeName) || ctx.expectedUiPid == 0) {
        LogE(L"worker 参数非法：缺少 --pipe 或 --parent-pid（拒绝进入交互模式）");
        return 2;
    }

    unsigned long long workerStart = 0;
    GetProcessCreationTime(GetCurrentProcessId(), &workerStart);

    std::wstring sddlErr;
    const std::wstring sddl = BuildSddl(ctx, &sddlErr);
    if (sddl.empty()) {
        LogE(L"构建管道 SDDL 失败：" + sddlErr);
        return 3;
    }

    DWORD createErr = 0;
    HANDLE pipe = CreateServerPipe(ctx.pipeName, sddl, &createErr);
    if (pipe == INVALID_HANDLE_VALUE) {
        LogE(L"创建命名管道失败：" + ErrorWithCode(createErr));
        return 3;
    }
    LogI(L"worker 已就绪，管道 " + MaskSecret(ctx.pipeName) + L"，等待 UI 连接");

    if (!ConnectServer(pipe, 60000)) {
        LogE(L"60 秒内没有客户端连接，worker 退出");
        CloseHandle(pipe);
        return 3;
    }

    std::wstring why;
    if (!VerifyClient(pipe, ctx, workerStart, &why)) {
        LogE(L"客户端身份校验失败，断连：" + why);
        CloseHandle(pipe);
        return 3;
    }

    // 首帧必须是 hello（不是 ping）
    std::string frame;
    if (!ReadFrame(pipe, &frame, kHelloTimeoutMs)) {
        LogE(L"握手超时：第一帧不是 hello");
        CloseHandle(pipe);
        return 3;
    }
    IpcRequest hello;
    std::string jerr;
    if (!DecodeRequest(frame, &hello, &jerr) || hello.op != IpcOp::Hello) {
        LogE(L"首帧不是 hello，断连");
        CloseHandle(pipe);
        return 3;
    }
    json::Value hv;
    if (!json::Parse(frame, &hv, nullptr)) {
        CloseHandle(pipe);
        return 3;
    }
    const int ver = static_cast<int>(hv.GetNumber("ver", -1));
    if (ver != kProtocolVersion) {
        json::Value err = proto::ErrorResponse(hello.id, kExitUnlockFailed,
            L"执行组件版本不一致（协议 v" + std::to_wstring(ver) + L"，期望 v" +
            std::to_wstring(kProtocolVersion) + L"），请重启程序。");
        WriteFrame(pipe, json::Dump(err));
        LogE(L"协议版本不匹配：" + std::to_wstring(ver));
        CloseHandle(pipe);
        return 3;
    }
    {
        json::Value ack = json::Value::MakeObject();
        ack.Set("id", json::Value::MakeNumber(static_cast<double>(hello.id)));
        ack.Set("ok", json::Value::MakeBool(true));
        ack.Set("ver", json::Value::MakeNumber(kProtocolVersion));
        WriteFrame(pipe, json::Dump(ack));
    }
    LogI(L"握手完成，协议版本 " + std::to_wstring(kProtocolVersion));

    int exitCode = 0;
    for (;;) {
        g_cancel.store(false);
        frame.clear();
        if (!ReadFrame(pipe, &frame, kIdleTimeoutMs)) {
            const DWORD e = GetLastError();
            if (e == ERROR_TIMEOUT) {
                LogI(L"空闲超过 5 分钟，worker 自行退出");
            } else {
                LogI(L"管道读失败，worker 退出：" + ErrorWithCode(e));
            }
            break;
        }

        IpcRequest req;
        if (!DecodeRequest(frame, &req, &jerr)) {
            LogE(L"请求解析失败：" + Utf8ToUtf16(jerr));
            WriteFrame(pipe, json::Dump(proto::ErrorResponse(0, kExitBadArgs, L"请求格式非法")));
            break;
        }
        if (req.op == IpcOp::Invalid) {
            // 未知 op 一律拒绝并断连，不存在 exec 类入口
            LogE(L"未知 op，拒绝并断连");
            WriteFrame(pipe, json::Dump(proto::ErrorResponse(req.id, kExitBadArgs, L"未知操作")));
            break;
        }

        LogI(L"收到请求 op=" + Utf8ToUtf16(OpName(req.op)) + L" items=" +
             std::to_wstring(req.items.size()));

        switch (req.op) {
            case IpcOp::Hello:
            case IpcOp::Ping: {
                json::Value ack = json::Value::MakeObject();
                ack.Set("id", json::Value::MakeNumber(static_cast<double>(req.id)));
                ack.Set("ok", json::Value::MakeBool(true));
                ack.Set("ver", json::Value::MakeNumber(kProtocolVersion));
                ack.Set("evt", json::Value::MakeString("pong"));
                WriteFrame(pipe, json::Dump(ack));
                break;
            }
            case IpcOp::Scan:
                HandleScan(pipe, req, ctx);
                break;
            case IpcOp::Unlock:
            case IpcOp::Delete:
            case IpcOp::Rename:
            case IpcOp::Move:
            case IpcOp::Copy:
            case IpcOp::Reboot:
                HandleFileOp(pipe, req, ctx);
                break;
            case IpcOp::Cancel:
                g_cancel.store(true);
                WriteFrame(pipe, json::Dump(proto::ErrorResponse(req.id, kExitOk, L"已请求取消")));
                break;
            case IpcOp::Shutdown: {
                json::Value ack = json::Value::MakeObject();
                ack.Set("id", json::Value::MakeNumber(static_cast<double>(req.id)));
                ack.Set("ok", json::Value::MakeBool(true));
                WriteFrame(pipe, json::Dump(ack));
                CloseHandle(pipe);
                return 0;
            }
            default:
                WriteFrame(pipe, json::Dump(proto::ErrorResponse(req.id, kExitBadArgs, L"不支持的操作")));
                break;
        }
    }

    CloseHandle(pipe);
    return exitCode;
}

}  // namespace ul
