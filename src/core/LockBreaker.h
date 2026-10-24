#pragma once
// 分级拆锁（方案 4.4）：副作用从小到大，任何一级成功就停。
//   Level 0 正常删除 / Level 1 POSIX 语义删除 / Level 2 关闭远程句柄
//   Level 3 卸映像（P2，未实现）/ Level 4 终止进程 / Level 5 重启后执行
#include <windows.h>
#include <string>
#include <vector>

namespace ul {

// FILE_DISPOSITION_INFO_EX（老 SDK 没有就自己定义）；21 是 FileDispositionInfoEx 的信息类
struct FILE_DISPOSITION_INFO_EX_ {
    DWORD Flags;
};
constexpr int kFileDispositionInfoEx = 21;

constexpr DWORD kDispositionDelete        = 0x00000001;
constexpr DWORD kDispositionPosixSemantics = 0x00000002;
constexpr DWORD kDispositionIgnoreReadonly = 0x00000010;

class LockBreaker {
public:
    // Level 1：POSIX 语义删除。不能绕过共享冲突（那是 CreateFile 阶段内核判定的）
    static bool PosixDelete(const std::wstring& path, DWORD* err = nullptr);

    // 清只读属性（Level 0 的前置）
    static bool ClearReadOnly(const std::wstring& path);

    // Level 2：关闭远程句柄。返回成功关闭的句柄数
    static int CloseHandlesForPath(const std::wstring& path, int rounds = 3,
                                   std::vector<DWORD>* touchedPids = nullptr,
                                   DWORD* lastError = nullptr);

    static bool CloseRemoteHandle(DWORD pid, HANDLE hRemote);

    // Level 4：终止进程（先过黑名单；svchost/explorer 由调用方二次确认）
    static bool KillProcess(DWORD pid, bool confirmed, std::wstring* message);

    static bool IsHardDenyProcessName(const std::wstring& name);
    static bool IsConfirmProcessName(const std::wstring& name);

    // 进程列表（用于展示将被终止的对象）
    static std::vector<std::wstring> DescribeProcesses(const std::vector<DWORD>& pids);
};

}  // namespace ul
