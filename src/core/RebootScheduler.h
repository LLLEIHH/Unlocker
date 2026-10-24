#pragma once
// Level 5 重启兜底（方案 4.4）。smss.exe 在用户态进程启动之前处理，所以必定成功。
// 路径必须是完整 Win32 路径，且调用者具备 SeRestorePrivilege。
#include <windows.h>
#include <string>

namespace ul {

class RebootScheduler {
public:
    static bool ScheduleDelete(const std::wstring& path, DWORD* err = nullptr);
    static bool ScheduleMove(const std::wstring& from, const std::wstring& to, DWORD* err = nullptr);

    // 读取 PendingFileRenameOperations，确认写入成功（T13 的验收点）
    static bool IsScheduled(const std::wstring& path);
    static std::wstring Explain();
};

}  // namespace ul
