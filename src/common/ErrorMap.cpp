#include "common/ErrorMap.h"

#include <cstdio>
#include <vector>

namespace ul {

const wchar_t* OccupancyText(OccupancyState s) {
    switch (s) {
        case OccupancyState::Free:           return L"可操作";
        case OccupancyState::Opened:         return L"已打开";
        case OccupancyState::Locked:         return L"已锁定";
        case OccupancyState::ImageMapped:    return L"映像占用";
        case OccupancyState::AccessDenied:   return L"权限不足";
        case OccupancyState::LockedByKernel: return L"驱动锁定";
        case OccupancyState::NotFound:       return L"文件不存在";
        case OccupancyState::Error:          return L"无法访问";
    }
    return L"未知";
}

const wchar_t* OccupancyExplain(OccupancyState s) {
    switch (s) {
        case OccupancyState::Free:           return L"没有进程占用，可直接操作";
        case OccupancyState::Opened:         return L"有进程打开了它，但允许删除";
        case OccupancyState::Locked:         return L"有进程不允许删除，需要先释放句柄";
        case OccupancyState::ImageMapped:    return L"映像已被映射到进程，关闭句柄无效，只能结束进程或重启后删除";
        case OccupancyState::AccessDenied:   return L"权限或 ACL 问题，不是占用，需要管理员权限";
        case OccupancyState::LockedByKernel: return L"未找到持有进程，可能由安全软件或文件系统驱动锁定";
        case OccupancyState::NotFound:       return L"文件已经不存在";
        case OccupancyState::Error:          return L"路径非法、设备未就绪或未知错误";
    }
    return L"";
}

std::wstring Win32Message(DWORD err) {
    // 高频错误给中文，避免英文系统消息打断中文界面
    switch (err) {
        case ERROR_SUCCESS:               return L"成功";
        case ERROR_FILE_NOT_FOUND:        return L"文件不存在";
        case ERROR_PATH_NOT_FOUND:        return L"路径不存在";
        case ERROR_ACCESS_DENIED:         return L"拒绝访问（权限不足）";
        case ERROR_SHARING_VIOLATION:     return L"文件正被占用（共享冲突）";
        case ERROR_LOCK_VIOLATION:        return L"文件区域被锁定";
        case ERROR_INVALID_NAME:          return L"文件名或路径格式非法";
        case ERROR_BAD_PATHNAME:          return L"路径无效";
        case ERROR_INVALID_PARAMETER:     return L"参数无效";
        case ERROR_DIR_NOT_EMPTY:         return L"目录非空";
        case ERROR_NOT_SAME_DEVICE:       return L"不在同一个卷上";
        case ERROR_DISK_FULL:             return L"磁盘空间不足";
        case ERROR_WRITE_PROTECT:         return L"介质写保护";
        case ERROR_CANCELLED:             return L"操作已取消";
        case ERROR_PRIVILEGE_NOT_HELD:    return L"缺少所需特权";
        case ERROR_INVALID_HANDLE:        return L"句柄无效";
        case ERROR_PROCESS_ABORTED:       return L"进程已终止";
        case ERROR_FILE_TOO_LARGE:        return L"文件过大";
        case ERROR_NOT_SUPPORTED:         return L"该操作不受支持";
        case ERROR_FILENAME_EXCED_RANGE:  return L"路径过长";
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
    if (out.empty()) out = L"未知错误";
    return out;
}

std::wstring ErrorWithCode(DWORD err) {
    wchar_t buf[16];
    swprintf_s(buf, L"0x%08lX", err);
    return Win32Message(err) + L"（" + buf + L"）";
}

}  // namespace ul
