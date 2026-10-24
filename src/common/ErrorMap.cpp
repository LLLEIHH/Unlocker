#include "common/ErrorMap.h"

#include <cstdio>
#include <vector>

#include "common/Lang.h"

namespace ul {

const wchar_t* OccupancyText(OccupancyState s) {
    switch (s) {
        case OccupancyState::Free:           return T(L"可操作", L"Ready");
        case OccupancyState::Opened:         return T(L"已打开", L"Open");
        case OccupancyState::Locked:         return T(L"已锁定", L"Locked");
        case OccupancyState::ImageMapped:    return T(L"映像占用", L"Mapped");
        case OccupancyState::AccessDenied:   return T(L"权限不足", L"Denied");
        case OccupancyState::LockedByKernel: return T(L"驱动锁定", L"Kernel lock");
        case OccupancyState::NotFound:       return T(L"文件不存在", L"Missing");
        case OccupancyState::Error:          return T(L"无法访问", L"Inaccessible");
    }
    return T(L"未知", L"Unknown");
}

const wchar_t* OccupancyExplain(OccupancyState s) {
    switch (s) {
        case OccupancyState::Free:
            return T(L"没有进程占用，可直接操作",
                     L"No process is using it; you can act on it directly");
        case OccupancyState::Opened:
            return T(L"有进程打开了它，但允许删除",
                     L"A process has it open, but deletion is allowed");
        case OccupancyState::Locked:
            return T(L"有进程不允许删除，需要先释放句柄",
                     L"A process blocks deletion; the handle must be released first");
        case OccupancyState::ImageMapped:
            return T(L"映像已被映射到进程，关闭句柄无效，只能结束进程或重启后删除",
                     L"The image is mapped into a process; closing handles won't help, "
                     L"end the process or delete after reboot");
        case OccupancyState::AccessDenied:
            return T(L"权限或 ACL 问题，不是占用，需要管理员权限",
                     L"Access/ACL problem rather than a lock; administrator rights required");
        case OccupancyState::LockedByKernel:
            return T(L"未找到持有进程，可能由安全软件或文件系统驱动锁定",
                     L"No owner process found; likely locked by security software or a file "
                     L"system driver");
        case OccupancyState::NotFound:
            return T(L"文件已经不存在", L"The file no longer exists");
        case OccupancyState::Error:
            return T(L"路径非法、设备未就绪或未知错误",
                     L"Invalid path, device not ready, or unknown error");
    }
    return L"";
}

std::wstring Win32Message(DWORD err) {
    // 高频错误给本地化文案，避免系统消息语言和界面语言打架
    switch (err) {
        case ERROR_SUCCESS:               return T(L"成功", L"Success");
        case ERROR_FILE_NOT_FOUND:        return T(L"文件不存在", L"File not found");
        case ERROR_PATH_NOT_FOUND:        return T(L"路径不存在", L"Path not found");
        case ERROR_ACCESS_DENIED:         return T(L"拒绝访问（权限不足）", L"Access denied");
        case ERROR_SHARING_VIOLATION:     return T(L"文件正被占用（共享冲突）",
                                                  L"File is in use (sharing violation)");
        case ERROR_LOCK_VIOLATION:        return T(L"文件区域被锁定", L"File region is locked");
        case ERROR_INVALID_NAME:          return T(L"文件名或路径格式非法",
                                                  L"Invalid file name or path");
        case ERROR_BAD_PATHNAME:          return T(L"路径无效", L"Invalid path");
        case ERROR_INVALID_PARAMETER:     return T(L"参数无效", L"Invalid parameter");
        case ERROR_DIR_NOT_EMPTY:         return T(L"目录非空", L"Directory is not empty");
        case ERROR_NOT_SAME_DEVICE:       return T(L"不在同一个卷上", L"Not on the same volume");
        case ERROR_DISK_FULL:             return T(L"磁盘空间不足", L"Disk is full");
        case ERROR_WRITE_PROTECT:         return T(L"介质写保护", L"Media is write-protected");
        case ERROR_CANCELLED:             return T(L"操作已取消", L"Operation cancelled");
        case ERROR_PRIVILEGE_NOT_HELD:    return T(L"缺少所需特权", L"Required privilege not held");
        case ERROR_INVALID_HANDLE:        return T(L"句柄无效", L"Invalid handle");
        case ERROR_PROCESS_ABORTED:       return T(L"进程已终止", L"Process was terminated");
        case ERROR_FILE_TOO_LARGE:        return T(L"文件过大", L"File is too large");
        case ERROR_NOT_SUPPORTED:         return T(L"该操作不受支持", L"Not supported");
        case ERROR_FILENAME_EXCED_RANGE:  return T(L"路径过长", L"Path is too long");
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
    if (out.empty()) out = T(L"未知错误", L"Unknown error");
    return out;
}

std::wstring ErrorWithCode(DWORD err) {
    wchar_t buf[16];
    swprintf_s(buf, L"0x%08lX", err);
    return Tf(L"%s（%s）", L"%s (%s)", Win32Message(err).c_str(), buf);
}

}  // namespace ul
