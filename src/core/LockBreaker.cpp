#include "core/LockBreaker.h"

#include <algorithm>
#include <set>

#include "common/ErrorMap.h"
#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/HandleScanner.h"
#include "core/PathResolver.h"
#include "core/PolicyGuard.h"

namespace ul {
namespace {

// 一档：绝对禁止，连确认入口都不给
const wchar_t* kHardDenyProc[] = {
    L"System", L"Registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
    L"winlogon.exe", L"services.exe", L"lsass.exe", L"MsMpEng.exe",
};

// 二档：高危但常见，降为「需确认」。svchost 恰恰是文件删不掉最常见的元凶，
// 一刀切禁止会让工具在最典型的场景失效。
const wchar_t* kConfirmProc[] = { L"svchost.exe", L"explorer.exe" };

std::wstring BaseName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L'\\');
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

}  // namespace

bool LockBreaker::ClearReadOnly(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (!(attr & FILE_ATTRIBUTE_READONLY)) return true;
    return SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY) != FALSE;
}

bool LockBreaker::PosixDelete(const std::wstring& path, DWORD* err) {
    HANDLE h = CreateFileW(path.c_str(),
                           DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = GetLastError();     // 真正的共享冲突在这里就挂了，POSIX 语义救不了
        return false;
    }

    FILE_DISPOSITION_INFO_EX_ di{};
    di.Flags = kDispositionDelete | kDispositionPosixSemantics | kDispositionIgnoreReadonly;
    const BOOL ok = SetFileInformationByHandle(h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileDispositionInfoEx),
                                               &di, sizeof(di));
    const DWORD e = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (err) *err = e;
    return ok != FALSE;
}

bool LockBreaker::CloseRemoteHandle(DWORD pid, HANDLE hRemote) {
    HANDLE hProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    if (!hProc) return false;

    HANDLE dup = nullptr;
    // 关键：DUPLICATE_CLOSE_SOURCE —— 复制成功的同时源句柄被关闭。
    // 只关自己这份副本是错的：那只是减少引用计数，源进程依然持有 FILE_OBJECT，文件照样锁着。
    BOOL ok = DuplicateHandle(hProc, hRemote, GetCurrentProcess(), &dup,
                              0, FALSE, DUPLICATE_CLOSE_SOURCE);
    if (ok && dup && dup != INVALID_HANDLE_VALUE) CloseHandle(dup);
    CloseHandle(hProc);
    return ok != FALSE;
}

int LockBreaker::CloseHandlesForPath(const std::wstring& path, int rounds,
                                     std::vector<DWORD>* touchedPids, DWORD* lastError) {
    int closed = 0;
    if (lastError) *lastError = ERROR_SUCCESS;

    for (int round = 0; round < rounds; ++round) {
        std::vector<DWORD> skipped;
        auto owners = HandleScanner::FindOwners(path, &skipped, nullptr);
        if (owners.empty()) break;

        int closedThisRound = 0;
        for (const auto& o : owners) {
            if (CloseRemoteHandle(o.proc.pid, o.handleValue)) {
                ++closedThisRound;
                ++closed;
                if (touchedPids) touchedPids->push_back(o.proc.pid);
            } else if (lastError) {
                *lastError = GetLastError();
            }
        }
        if (closedThisRound == 0) break;
        Sleep(80);   // 等 IRP 收尾，别用 0
    }
    return closed;
}

bool LockBreaker::IsHardDenyProcessName(const std::wstring& name) {
    for (const wchar_t* n : kHardDenyProc) {
        if (IEquals(name, n)) return true;
    }
    return false;
}

bool LockBreaker::IsConfirmProcessName(const std::wstring& name) {
    for (const wchar_t* n : kConfirmProc) {
        if (IEquals(name, n)) return true;
    }
    return false;
}

std::vector<std::wstring> LockBreaker::DescribeProcesses(const std::vector<DWORD>& pids) {
    std::vector<std::wstring> out;
    for (DWORD pid : pids) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) {
            out.push_back(L"PID " + std::to_wstring(pid) + L"（无法读取进程信息）");
            continue;
        }
        wchar_t buf[32768]{};
        DWORD len = static_cast<DWORD>(std::size(buf));
        std::wstring path;
        if (QueryFullProcessImageNameW(h, 0, buf, &len)) path.assign(buf, len);
        CloseHandle(h);
        out.push_back((path.empty() ? std::wstring(L"[未知进程]") : BaseName(path)) +
                      L" (PID " + std::to_wstring(pid) + L")");
    }
    return out;
}

bool LockBreaker::KillProcess(DWORD pid, bool confirmed, std::wstring* message) {
    if (pid == GetCurrentProcessId()) {
        if (message) *message = L"不能结束本程序自身。";
        return false;
    }

    // 先拿完整映像路径再过黑名单：进程名随手就能改，不能只看名字
    std::wstring fullPath;
    HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (probe) {
        wchar_t buf[32768]{};
        DWORD len = static_cast<DWORD>(std::size(buf));
        if (QueryFullProcessImageNameW(probe, 0, buf, &len)) fullPath.assign(buf, len);
        CloseHandle(probe);
    }

    if (fullPath.empty()) {
        // PPL / 受保护进程：OpenProcess 直接失败时视为 Deny，不要重试
        if (message) {
            *message = L"该进程受系统保护（无法读取映像路径），拒绝结束它。";
        }
        return false;
    }

    PolicyResult policy = PolicyGuard::EvaluateProcess(fullPath, pid);
    if (policy.Denied()) {
        if (message) *message = PolicyGuard::ThreePartMessage(policy);
        LogAudit(L"DENY", PolicyGuard::RuleName(policy.ruleId), fullPath, policy.canonical,
                 L"KillProcess", L"worker");
        return false;
    }

    const std::wstring name = BaseName(fullPath);
    if (IsHardDenyProcessName(name)) {
        if (message) {
            *message = name + L" 属于系统关键进程，结束它会导致系统蓝屏或立即重启。此规则为内置保护，无法关闭。";
        }
        return false;
    }

    if (IsConfirmProcessName(name) && !confirmed) {
        if (message) {
            *message = name + L" 承载系统服务或桌面外壳，结束它会连带停掉同组服务。请确认后重试。";
        }
        return false;
    }

    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, pid);
    if (!h) {
        if (message) *message = L"无法打开进程（" + ErrorWithCode(GetLastError()) + L"）";
        return false;
    }
    const BOOL ok = TerminateProcess(h, 1);
    const DWORD e = ok ? ERROR_SUCCESS : GetLastError();
    if (ok) WaitForSingleObject(h, 3000);
    CloseHandle(h);

    if (!ok) {
        if (message) *message = name + L" 终止失败：" + ErrorWithCode(e);
        return false;
    }
    LogI(L"已终止进程 " + name + L" (PID " + std::to_wstring(pid) + L")");
    if (message) *message = L"已终止 " + name + L" (PID " + std::to_wstring(pid) + L")";
    return true;
}

}  // namespace ul
