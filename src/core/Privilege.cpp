#include "core/Privilege.h"

#include <sddl.h>

#include <vector>

#include "common/Log.h"

namespace ul {
namespace {

// AdjustTokenPrivileges 的返回值不能当作「已启用」的证据
bool SetPrivilegeEnabled(HANDLE tok, const TOKEN_PRIVILEGES& tp) {
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(tok, FALSE, const_cast<TOKEN_PRIVILEGES*>(&tp), 0, nullptr, nullptr)) {
        return false;
    }
    return GetLastError() == ERROR_SUCCESS;
}

}  // namespace

bool EnablePrivilege(const wchar_t* name, bool* assigned) {
    if (assigned) *assigned = false;

    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        LogW(L"OpenProcessToken 失败: " + std::to_wstring(GetLastError()));
        return false;
    }

    LUID luid{};
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, name, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        if (SetPrivilegeEnabled(tok, tp)) {
            ok = (GetLastError() != ERROR_NOT_ALL_ASSIGNED);
            if (assigned) *assigned = ok;
        }
    }
    CloseHandle(tok);

    if (!ok) {
        LogW(std::wstring(L"特权未启用: ") + name + L"（" + std::to_wstring(GetLastError()) + L"）");
    }
    return ok;
}

// 内部辅助已上移
PrivilegeReport EnableWorkerPrivileges() {
    PrivilegeReport r;
    r.debug   = EnablePrivilege(SE_DEBUG_NAME);
    r.backup  = EnablePrivilege(SE_BACKUP_NAME);
    r.restore = EnablePrivilege(SE_RESTORE_NAME);
    LogI(L"worker 特权：debug=" + std::to_wstring(r.debug) + L" backup=" + std::to_wstring(r.backup) +
         L" restore=" + std::to_wstring(r.restore));
    return r;
}

bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD len = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &len);
    CloseHandle(tok);
    return ok && e.TokenIsElevated != 0;
}

bool IsCurrentProcessElevated() { return IsElevated(); }

namespace {

std::wstring SidToString(PSID sid) {
    if (!sid) return {};
    LPWSTR str = nullptr;
    if (!ConvertSidToStringSidW(sid, &str)) return {};
    std::wstring out = str;
    LocalFree(str);
    return out;
}

}  // namespace

std::wstring CurrentUserSidString() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return {};
    BYTE buf[256]{};
    DWORD len = 0;
    std::wstring out;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len)) {
        auto* tu = reinterpret_cast<TOKEN_USER*>(buf);
        out = SidToString(tu->User.Sid);
    }
    CloseHandle(tok);
    return out;
}

std::wstring UserSidStringOfProcess(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    HANDLE tok = nullptr;
    std::wstring out;
    if (OpenProcessToken(h, TOKEN_QUERY, &tok)) {
        BYTE buf[256]{};
        DWORD len = 0;
        if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len)) {
            auto* tu = reinterpret_cast<TOKEN_USER*>(buf);
            out = SidToString(tu->User.Sid);
        }
        CloseHandle(tok);
    }
    CloseHandle(h);
    return out;
}

bool GetProcessCreationTime(DWORD pid, unsigned long long* out) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    FILETIME c{}, e{}, k{}, u{};
    const bool ok = GetProcessTimes(h, &c, &e, &k, &u) != FALSE;
    CloseHandle(h);
    if (!ok) return false;
    if (out) {
        *out = (static_cast<unsigned long long>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    }
    return true;
}

}  // namespace ul
