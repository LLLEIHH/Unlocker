#include "common/ErrorMap.h"

#include <cstdio>
#include <vector>

#include "common/Lang.h"

namespace ul {

const wchar_t* OccupancyText(OccupancyState s) {
    switch (s) {
        case OccupancyState::Free:           return T(Str::OccupancyReady);
        case OccupancyState::Opened:         return T(Str::OccupancyOpen);
        case OccupancyState::Locked:         return T(Str::OccupancyLocked);
        case OccupancyState::ImageMapped:    return T(Str::OccupancyMapped);
        case OccupancyState::AccessDenied:   return T(Str::OccupancyDenied);
        case OccupancyState::LockedByKernel: return T(Str::OccupancyKernelLock);
        case OccupancyState::NotFound:       return T(Str::OccupancyMissing);
        case OccupancyState::Error:          return T(Str::OccupancyInaccessible);
    }
    return T(Str::OccupancyUnknown);
}

const wchar_t* OccupancyExplain(OccupancyState s) {
    switch (s) {
        case OccupancyState::Free:           return T(Str::ExplainFree);
        case OccupancyState::Opened:         return T(Str::ExplainOpen);
        case OccupancyState::Locked:         return T(Str::ExplainLocked);
        case OccupancyState::ImageMapped:    return T(Str::ExplainMapped);
        case OccupancyState::AccessDenied:   return T(Str::ExplainDenied);
        case OccupancyState::LockedByKernel: return T(Str::ExplainKernelLock);
        case OccupancyState::NotFound:       return T(Str::ExplainMissing);
        case OccupancyState::Error:          return T(Str::ExplainError);
    }
    return L"";
}

std::wstring Win32Message(DWORD err) {
    // 高频错误给本地化文案，避免系统消息语言和界面语言打架
    switch (err) {
        case ERROR_SUCCESS:               return T(Str::ErrSuccess);
        case ERROR_FILE_NOT_FOUND:        return T(Str::ErrFileNotFound);
        case ERROR_PATH_NOT_FOUND:        return T(Str::ErrPathNotFound);
        case ERROR_ACCESS_DENIED:         return T(Str::ErrAccessDenied);
        case ERROR_SHARING_VIOLATION:     return T(Str::ErrSharing);
        case ERROR_LOCK_VIOLATION:        return T(Str::ErrLock);
        case ERROR_INVALID_NAME:          return T(Str::ErrInvalidName);
        case ERROR_BAD_PATHNAME:          return T(Str::ErrBadPath);
        case ERROR_INVALID_PARAMETER:     return T(Str::ErrInvalidParam);
        case ERROR_DIR_NOT_EMPTY:         return T(Str::ErrDirNotEmpty);
        case ERROR_NOT_SAME_DEVICE:       return T(Str::ErrNotSameDevice);
        case ERROR_DISK_FULL:             return T(Str::ErrDiskFull);
        case ERROR_WRITE_PROTECT:         return T(Str::ErrWriteProtect);
        case ERROR_CANCELLED:             return T(Str::ErrCancelled);
        case ERROR_PRIVILEGE_NOT_HELD:    return T(Str::ErrPrivilege);
        case ERROR_INVALID_HANDLE:        return T(Str::ErrInvalidHandle);
        case ERROR_PROCESS_ABORTED:       return T(Str::ErrProcessAborted);
        case ERROR_FILE_TOO_LARGE:        return T(Str::ErrFileTooLarge);
        case ERROR_NOT_SUPPORTED:         return T(Str::ErrNotSupported);
        case ERROR_FILENAME_EXCED_RANGE:  return T(Str::ErrPathTooLong);
        default: break;
    }

    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring out;
    if (n && buf) {
        out.assign(buf, n);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' ')) out.pop_back();
    }
    if (buf) LocalFree(buf);
    if (out.empty()) out = T(Str::ErrUnknown);
    return out;
}

std::wstring ErrorWithCode(DWORD err) {
    wchar_t buf[16];
    swprintf_s(buf, L"0x%08lX", err);
    return Tf(Str::ErrWithCodeFmt, Win32Message(err).c_str(), buf);
}

}  // namespace ul
