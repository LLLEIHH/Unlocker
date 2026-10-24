#pragma once
// NTAPI 声明与结构体。
// 只用于「查询」：枚举句柄、取对象名、挂起进程。没有任何写操作。
#include <windows.h>
#include <winternl.h>
#include <string>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif
#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

// 方案 10.1：SystemExtendedHandleInformation = 64，不能用 SystemHandleInformation(16)
constexpr ULONG kSystemExtendedHandleInformation = 64;
constexpr ULONG kObjectNameInformation = 1;
constexpr ULONG kSystemProcessInformation = 5;

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_ {
    PVOID     Object;
    ULONG_PTR UniqueProcessId;
    HANDLE    HandleValue;
    ULONG     GrantedAccess;
    USHORT    CreatorBackTraceIndex;
    USHORT    ObjectTypeIndex;
    ULONG     HandleAttributes;
    ULONG     Reserved;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_;

typedef struct _SYSTEM_HANDLE_INFORMATION_EX_ {
    ULONG_PTR                            NumberOfHandles;
    ULONG_PTR                            Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_   Handles[1];
} SYSTEM_HANDLE_INFORMATION_EX_;

namespace nt {

using PfnNtQuerySystemInformation = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using PfnNtQueryObject           = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using PfnNtSuspendProcess        = NTSTATUS(NTAPI*)(HANDLE);
using PfnNtResumeProcess         = NTSTATUS(NTAPI*)(HANDLE);

// 延迟解析 ntdll 导出，失败则相关能力整体降级（扫描会返回空而不是崩溃）。
bool Init();
PfnNtQuerySystemInformation QuerySystemInformation();
PfnNtQueryObject            QueryObject();
PfnNtSuspendProcess         SuspendProcess();
PfnNtResumeProcess          ResumeProcess();

}  // namespace nt
