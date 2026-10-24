#pragma once
// UI/CLI 侧：拉起 worker、发请求、读进度（方案 4.10.4）。
// UAC 弹窗会一直阻塞到用户点确认，所以 EnsureStarted 必须在工作线程调用。
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

#include "ipc/Json.h"
#include "ipc/Protocol.h"

namespace ul {

class PipeClient {
public:
    using ResponseFn = std::function<void(const json::Value&)>;

    PipeClient() = default;
    ~PipeClient();
    PipeClient(const PipeClient&)            = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // 拉起 worker（首次会弹 UAC）并完成 hello 握手。已连接则直接复用。
    bool EnsureStarted(std::wstring* err);

    // 发一条请求，逐帧回调（progress 与最终结果都会进 fn）。
    bool Request(const IpcRequest& req, const ResponseFn& fn, std::wstring* err,
                 DWORD timeoutMs = 120000);

    void Stop();
    bool Connected() const { return pipe_ != nullptr && hello_; }
    bool WorkerAlive();
    DWORD WorkerPid() const;
    std::wstring PipeName() const { return pipeName_; }

    static std::wstring MakePipeName();
    static std::wstring SelfExePath();

private:
    bool Connect(std::wstring* err);
    void Cleanup();

    HANDLE       pipe_    = nullptr;
    HANDLE       proc_    = nullptr;
    std::wstring pipeName_;
    uint64_t     nextId_  = 1;
    bool         hello_   = false;
};

}  // namespace ul
