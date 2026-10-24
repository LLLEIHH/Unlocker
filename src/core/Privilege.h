#pragma once
// 令牌特权调整（只对 worker 生效）与提权状态查询。
#include <windows.h>
#include <string>

namespace ul {

// 返回 true 表示特权已启用。注意 AdjustTokenPrivileges 即使在特权不存在时也返回 TRUE，
// 必须检查 GetLastError() == ERROR_NOT_ALL_ASSIGNED（方案 4.1 的坑）。
bool EnablePrivilege(const wchar_t* name, bool* assigned = nullptr);

// worker 启动时调用：SeDebugPrivilege / SeBackupPrivilege / SeRestorePrivilege。
// SeRestore 才是 DELETE / 写访问的豁免者（v0.8 归因修正）。
struct PrivilegeReport {
    bool debug   = false;
    bool backup  = false;
    bool restore = false;
    bool Any() const { return debug || backup || restore; }
};
PrivilegeReport EnableWorkerPrivileges();

bool IsElevated();
bool IsCurrentProcessElevated();

// SID 字符串（S-1-5-...）。SID 不是秘密：它是授权目标，不是凭证。
// worker 侧必须从父进程令牌读取，不能只信命令行传来的值。
std::wstring CurrentUserSidString();
std::wstring UserSidStringOfProcess(DWORD pid);

// 进程创建时间（100ns，FILETIME 合并），用于三重身份校验防 PID 复用
bool GetProcessCreationTime(DWORD pid, unsigned long long* out);

}  // namespace ul
