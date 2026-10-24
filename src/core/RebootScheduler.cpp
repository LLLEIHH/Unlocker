#include "core/RebootScheduler.h"

#include <vector>

#include "common/StringUtil.h"
#include "core/PathResolver.h"

namespace ul {
namespace {

constexpr wchar_t kPendingKey[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";

bool ReadPending(std::vector<std::wstring>* entries) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPendingKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;

    DWORD type = 0;
    DWORD bytes = 0;
    bool ok = false;
    if (RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        type == REG_MULTI_SZ && bytes > 0) {
        std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, &type,
                             reinterpret_cast<BYTE*>(buf.data()), &bytes) == ERROR_SUCCESS) {
            for (size_t i = 0; i < buf.size();) {
                const wchar_t* s = buf.data() + i;
                const size_t len = wcslen(s);
                if (len == 0) break;
                if (entries) entries->emplace_back(s, len);
                i += len + 1;
            }
            ok = true;
        }
    }
    RegCloseKey(key);
    return ok;
}

}  // namespace

bool RebootScheduler::ScheduleDelete(const std::wstring& path, DWORD* err) {
    SetLastError(ERROR_SUCCESS);
    if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        if (err) *err = 0;
        return true;
    }
    // 常见失败：非管理员（没有 SeRestorePrivilege）或路径不是完整 Win32 路径
    const DWORD e = GetLastError();
    if (err) *err = e;
    return false;
}

bool RebootScheduler::ScheduleMove(const std::wstring& from, const std::wstring& to, DWORD* err) {
    SetLastError(ERROR_SUCCESS);
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_DELAY_UNTIL_REBOOT)) {
        if (err) *err = 0;
        return true;
    }
    const DWORD e = GetLastError();
    if (err) *err = e;
    return false;
}

bool RebootScheduler::IsScheduled(const std::wstring& path) {
    std::vector<std::wstring> entries;
    if (!ReadPending(&entries)) return false;
    const std::wstring key = ToUpper(PathResolver::Normalize(path));
    for (const auto& e : entries) {
        if (ToUpper(PathResolver::Normalize(e)) == key) return true;
    }
    return false;
}

std::wstring RebootScheduler::Explain() {
    return L"已排队到重启后执行：Windows 会在下次启动、任何用户态进程运行之前由 SMSS 处理该操作，所以必定成功。"
           L"在此之前文件仍然存在，属于正常现象。";
}

}  // namespace ul
