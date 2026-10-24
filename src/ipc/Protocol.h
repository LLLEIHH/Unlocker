#pragma once
// IPC 协议（方案 4.10.2）：长度前缀 + UTF-8 JSON，两端唯一真源。
//   [4 字节 little-endian 长度][JSON 正文]
// 第一帧必须是 hello，版本不匹配即断连 —— 用户升级后旧 worker 残留是最常见的协议错位来源。
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

#include "core/FileOps.h"
#include "core/HandleScanner.h"
#include "ipc/Json.h"

namespace ul {

constexpr int    kProtocolVersion = 3;
constexpr size_t kMaxMessageBytes = 16u * 1024 * 1024;   // 单条消息 > 16MB 直接拒绝
constexpr DWORD  kDefaultIoTimeoutMs = 30000;

enum class IpcOp {
    Invalid,
    Hello,
    Ping,
    Scan,
    Unlock,
    Delete,
    Rename,
    Move,
    Copy,
    Reboot,
    Cancel,
    Shutdown,
};

const char* OpName(IpcOp op);
IpcOp       ParseOp(const std::string& name);
bool        IsDangerousOp(IpcOp op);

// 启动上下文只属于 worker 进程，不进请求体
struct WorkerContext {
    std::wstring pipeName;          // 来自 --pipe
    DWORD        expectedUiPid = 0; // 来自 --parent-pid
    std::wstring uiUserSid;         // 从父进程令牌读出（或 --ui-sid），用于构建 DACL
    bool         quiet         = false;
};

struct IpcRequest {
    uint64_t                 id    = 0;
    IpcOp                    op    = IpcOp::Ping;
    std::vector<std::wstring> items;
    std::wstring             arg;       // rename 的新名 / move、copy 的目标目录
    bool                     force  = false;
    bool                     reboot = false;
    bool                     quiet  = false;
    std::string              caller = "GUI";
};

std::string EncodeRequest(const IpcRequest& req);
bool        DecodeRequest(const std::string& text, IpcRequest* req, std::string* err);

// 帧读写：两个方向都必须是 FILE_FLAG_OVERLAPPED 打开的句柄
bool ReadFrame(HANDLE pipe, std::string* json, DWORD timeoutMs);
bool WriteFrame(HANDLE pipe, const std::string& json, DWORD timeoutMs = kDefaultIoTimeoutMs);

const char* StateToken(OccupancyState s);
const char* OpCodeName(IpcOp op);

// 复用给 UI 与 CLI 的序列化
namespace proto {

json::Value ReportToJson(const FileReport& rep);
json::Value ItemResultToJson(const ItemResult& r);
json::Value ProgressEvent(uint64_t id, int cur, int total, const std::wstring& stage);
json::Value ErrorResponse(uint64_t id, int code, const std::wstring& msg);

}  // namespace proto

}  // namespace ul
