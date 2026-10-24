#include "ui/UiIpc.h"

#include <cstdint>
#include <thread>

#include "common/Log.h"
#include "common/StringUtil.h"

namespace ul {
namespace {

std::wstring PipeName() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return L"\\\\.\\pipe\\Unlocker.UI." + std::to_wstring(session);
}

std::string EncodePaths(const std::vector<std::wstring>& paths) {
    std::string out;
    for (const auto& p : paths) {
        out += Utf16ToUtf8(p);
        out.push_back('\n');
    }
    return out;
}

std::vector<std::wstring> DecodePaths(const std::string& payload) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start < payload.size()) {
        size_t nl = payload.find('\n', start);
        if (nl == std::string::npos) nl = payload.size();
        std::string line = payload.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(Utf8ToUtf16(line));
        start = nl + 1;
    }
    return out;
}

}  // namespace

std::wstring UiPipeName() { return PipeName(); }

bool SendPathsToRunningInstance(const std::vector<std::wstring>& paths, DWORD timeoutMs) {
    if (paths.empty()) return false;
    const std::wstring name = PipeName();

    // 冷启动竞态（计划 R-27）：第二个实例被拉起时，第一个实例可能还在 XAML 初始化，
    // 监听管道尚未 CreateNamedPipe，此时 WaitNamedPipeW 立刻返回 FALSE
    // （ERROR_FILE_NOT_FOUND / ERROR_SEM_TIMEOUT），路径会被静默丢掉，
    // 用户看到的现象是「窗口起来了，但列表是空的」。
    // 这里改成「总预算内轮询重试」：预算用完才算失败。
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        const DWORD remaining = deadline > GetTickCount64()
                                    ? static_cast<DWORD>(deadline - GetTickCount64())
                                    : 0;
        if (WaitNamedPipeW(name.c_str(), remaining ? remaining : 1)) break;
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_SEM_TIMEOUT && err != ERROR_PIPE_BUSY) {
            return false;  // 其它错误（权限等）重试也没用
        }
        if (GetTickCount64() >= deadline) return false;
        Sleep(40);
    }

    HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    const std::string payload = EncodePaths(paths);
    const uint32_t len = static_cast<uint32_t>(payload.size());
    DWORD wrote = 0;
    bool ok = WriteFile(h, &len, sizeof(len), &wrote, nullptr) && wrote == sizeof(len);
    if (ok) {
        ok = WriteFile(h, payload.data(), len, &wrote, nullptr) && wrote == len;
    }
    CloseHandle(h);
    return ok;
}

namespace {

void ListenerLoop(HWND hwnd, const std::function<void(std::vector<std::wstring>)>& callback) {
    const std::wstring name = PipeName();
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       4, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            LogE(L"UI 管道创建失败：" + std::to_wstring(GetLastError()));
            return;
        }
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            uint32_t len = 0;
            DWORD got = 0;
            if (ReadFile(pipe, &len, sizeof(len), &got, nullptr) && got == sizeof(len) &&
                len > 0 && len < 1024 * 1024) {
                std::string payload(len, '\0');
                DWORD total = 0;
                while (total < len) {
                    DWORD part = 0;
                    if (!ReadFile(pipe, payload.data() + total, len - total, &part, nullptr) || part == 0) break;
                    total += part;
                }
                auto paths = DecodePaths(payload);
                if (!paths.empty()) {
                    if (callback) {
                        callback(std::move(paths));
                    } else if (hwnd) {
                        auto* heap = new std::vector<std::wstring>(std::move(paths));
                        if (!PostMessageW(hwnd, WM_APP_SHELL_PATHS, 0, reinterpret_cast<LPARAM>(heap))) {
                            delete heap;
                        }
                    }
                }
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

}  // namespace

bool StartUiPipeListener(HWND hwnd) {
    if (!hwnd) return false;
    std::thread([hwnd] { ListenerLoop(hwnd, nullptr); }).detach();
    return true;
}

bool StartUiPipeListenerFn(std::function<void(std::vector<std::wstring>)> callback) {
    if (!callback) return false;
    std::thread([cb = std::move(callback)] { ListenerLoop(nullptr, cb); }).detach();
    return true;
}

}  // namespace ul
