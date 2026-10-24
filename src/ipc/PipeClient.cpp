#include "ipc/PipeClient.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <cstdlib>
#include <cstdio>

#include "common/ErrorMap.h"
#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/PathResolver.h"
#include "core/Privilege.h"

namespace ul {
namespace {

std::wstring RandomHex(size_t chars) {
    static const wchar_t* kHex = L"0123456789abcdef";
    std::wstring out;
    out.reserve(chars);

    unsigned char raw[32]{};
    if (BCryptGenRandom(nullptr, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // 极端情况下退回时间/计数器混合值：令牌只降低可见性，不当保密用
        const unsigned long long mix = GetTickCount64() ^ (static_cast<unsigned long long>(GetCurrentProcessId()) << 32);
        for (size_t i = 0; i < sizeof(raw); ++i) raw[i] = static_cast<unsigned char>((mix >> ((i % 8) * 8)) & 0xFF) ^ static_cast<unsigned char>(i * 37);
    }
    for (size_t i = 0; i < chars; ++i) {
        const unsigned char b = raw[(i / 2) % sizeof(raw)];
        out.push_back(kHex[(i % 2 == 0) ? (b & 0xF) : ((b >> 4) & 0xF)]);
    }
    return out;
}

}  // namespace

PipeClient::~PipeClient() { Cleanup(); }

std::wstring PipeClient::SelfExePath() {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return std::wstring(buf, n);
}

std::wstring PipeClient::MakePipeName() {
    // 令牌嵌进管道名（128 位随机）；命令行里只出现这一处，日志里打码
    return L"\\\\.\\pipe\\Unlocker.W." + RandomHex(32);
}

void PipeClient::Cleanup() {
    if (pipe_) {
        CloseHandle(pipe_);
        pipe_ = nullptr;
    }
    if (proc_) {
        CloseHandle(proc_);
        proc_ = nullptr;
    }
    hello_ = false;
    pipeName_.clear();
}

bool PipeClient::WorkerAlive() {
    if (!proc_) return false;
    return WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT;
}

DWORD PipeClient::WorkerPid() const {
    return proc_ ? GetProcessId(proc_) : 0;
}

void PipeClient::Stop() {
    if (pipe_ && hello_) {
        IpcRequest req;
        req.id = nextId_++;
        req.op = IpcOp::Shutdown;
        WriteFrame(pipe_, EncodeRequest(req), 3000);
    }
    Cleanup();
}

bool PipeClient::Connect(std::wstring* err) {
    // worker 启动 + UAC 确认需要时间；5 秒内连不上就杀掉重来（4.10.5）
    const ULONGLONG deadline = GetTickCount64() + 5000;
    DWORD lastErr = 0;
    while (GetTickCount64() < deadline) {
        if (WaitNamedPipeW(pipeName_.c_str(), 250)) {
            HANDLE h = CreateFileW(pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                // 消息模式（与服务端一致）
                DWORD mode = PIPE_READMODE_MESSAGE;
                SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
                pipe_ = h;
                return true;
            }
            lastErr = GetLastError();
        } else {
            lastErr = GetLastError();
        }
        if (proc_ && WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0) {
            if (err) *err = L"执行组件启动后立即退出";
            return false;
        }
        Sleep(100);
    }
    if (err) *err = L"执行组件启动失败（5 秒内未连上管道）：" + ErrorWithCode(lastErr);
    return false;
}

bool PipeClient::EnsureStarted(std::wstring* err) {
    if (pipe_ && hello_) return true;

    // 旧 worker 残留（例如升级后 exe 变过）：先关掉再重拉，不要直接报错给用户
    if (pipe_ || proc_) {
        Stop();
        Sleep(100);
    }

    pipeName_ = MakePipeName();

    const std::wstring self = SelfExePath();
    if (self.empty()) {
        if (err) *err = L"无法获取自身路径";
        return false;
    }
    const std::wstring uiSid = CurrentUserSidString();

    std::wstring params = L"--worker --pipe=\"" + pipeName_ + L"\" --parent-pid=" +
                          std::to_wstring(GetCurrentProcessId());
    if (!uiSid.empty()) params += L" --ui-sid=" + uiSid;

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = L"runas";
    sei.lpFile       = self.c_str();      // 必须是绝对路径，用 argv[0] 从 Shell 扩展拉起会失败
    sei.lpParameters = params.c_str();
    sei.nShow        = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        const DWORD e = GetLastError();
        if (e == ERROR_CANCELLED) {
            if (err) *err = L"已取消提权，无法执行操作";   // 正常路径，不当异常处理
        } else {
            if (err) *err = L"无法启动执行组件：" + ErrorWithCode(e);
        }
        return false;
    }
    proc_ = sei.hProcess;
    LogI(L"已拉起 worker PID=" + std::to_wstring(WorkerPid()) + L" 管道 " + MaskSecret(pipeName_));

    if (!Connect(err)) {
        if (proc_) {
            TerminateProcess(proc_, 1);
            CloseHandle(proc_);
            proc_ = nullptr;
        }
        return false;
    }

    // hello 握手：版本不匹配就断连
    IpcRequest hello;
    hello.id = nextId_++;
    hello.op = IpcOp::Hello;
    json::Value hv = json::Value::MakeObject();
    hv.Set("id", json::Value::MakeNumber(static_cast<double>(hello.id)));
    hv.Set("op", json::Value::MakeString("hello"));
    hv.Set("ver", json::Value::MakeNumber(kProtocolVersion));

    if (!WriteFrame(pipe_, json::Dump(hv), 15000)) {
        if (err) *err = L"握手发送失败";
        Cleanup();
        return false;
    }
    std::string frame;
    if (!ReadFrame(pipe_, &frame, 15000)) {
        if (err) *err = L"握手无响应（执行组件可能版本不匹配）";
        Cleanup();
        return false;
    }
    json::Value ack;
    std::string jerr;
    if (!json::Parse(frame, &ack, &jerr) || !ack.GetBool("ok", false)) {
        if (err) *err = L"握手失败：" + Utf8ToUtf16(ack.GetString("msg", jerr));
        Cleanup();
        return false;
    }
    const int ver = static_cast<int>(ack.GetNumber("ver", -1));
    if (ver != kProtocolVersion) {
        if (err) *err = L"执行组件版本不一致（v" + std::to_wstring(ver) + L" ≠ v" +
                        std::to_wstring(kProtocolVersion) + L"）";
        Cleanup();
        return false;
    }
    hello_ = true;

    // 空闲退出 5 分钟由 worker 自己管，这里只负责监控异常退出
    return true;
}

bool PipeClient::Request(const IpcRequest& reqIn, const ResponseFn& fn, std::wstring* err,
                         DWORD timeoutMs) {
    if (!EnsureStarted(err)) return false;

    IpcRequest req = reqIn;
    if (req.id == 0) req.id = nextId_++;

    if (!WriteFrame(pipe_, EncodeRequest(req), 15000)) {
        if (err) *err = L"发送请求失败：" + ErrorWithCode(GetLastError());
        Cleanup();
        return false;
    }

    for (;;) {
        std::string frame;
        if (!ReadFrame(pipe_, &frame, timeoutMs)) {
            if (err) {
                *err = (GetLastError() == ERROR_TIMEOUT) ? L"执行组件响应超时"
                                                         : L"读取响应失败：" + ErrorWithCode(GetLastError());
            }
            Cleanup();
            return false;
        }
        json::Value v;
        std::string jerr;
        if (!json::Parse(frame, &v, &jerr)) {
            if (err) *err = L"响应不是合法 JSON：" + Utf8ToUtf16(jerr);
            continue;
        }
        if (static_cast<uint64_t>(v.GetNumber("id", -1)) != req.id) continue;
        if (fn) fn(v);
        if (v.GetString("evt") == "progress") continue;
        return true;
    }
}

}  // namespace ul
