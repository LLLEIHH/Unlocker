#pragma once
#include <windows.h>
#include <string>

namespace ul {

// 方案 4.2：8 种占用状态。NotFound / Error 是终止态，不进解锁流程。
enum class OccupancyState {
    Free,
    Opened,
    Locked,
    ImageMapped,
    AccessDenied,
    LockedByKernel,
    NotFound,
    Error
};

const wchar_t* OccupancyText(OccupancyState s);        // 状态列文案
const wchar_t* OccupancyExplain(OccupancyState s);     // 一句话解释（给用户看的）

// Win32 / NTSTATUS -> 中文提示。未知错误保留原始错误码，不猜。
std::wstring Win32Message(DWORD err);
std::wstring ErrorWithCode(DWORD err);

// CLI 退出码（方案 4.9）。--force 只允许杀进程，不能覆盖黑名单。
enum ExitCode {
    kExitOk             = 0,
    kExitBadArgs        = 1,
    kExitNoOwner        = 2,
    kExitAccessDenied   = 3,
    kExitPolicyDenied   = 4,
    kExitUnlockFailed   = 5,
    kExitRebootRequired = 6,
    kExitNeedConfirm    = 7,
    kExitNeedElevation  = 8,
};

}  // namespace ul
