// MakeLock —— 造锁工具。没有稳定的锁源，后面每个功能都只能靠猜（方案 §11）。
//
//   MakeLock.exe <path> [seconds]      以「不含 FILE_SHARE_DELETE」的模式打开并持有
//   MakeLock.exe <path> --nosharerdelete  同上（默认行为）
//   MakeLock.exe <path> --sharedelete  以含 FILE_SHARE_DELETE 的模式打开（验证「已打开」而非「锁定」）
//   MakeLock.exe <dir>  --dir          以目录句柄方式持有（模拟 cmd 的工作目录）
//
// 默认持有 60 秒，按 Ctrl+C 提前退出。
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace {

void Print(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwprintf(fmt, ap);
    va_end(ap);
    wprintf(L"\n");
    fflush(stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        Print(L"用法: MakeLock.exe <路径> [--sharedelete] [--dir] [秒数]");
        return 1;
    }

    const std::wstring path = argv[1];
    bool shareDelete = false;
    bool asDir       = false;
    DWORD seconds    = 60;

    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--sharedelete") shareDelete = true;
        else if (a == L"--nosharerdelete" || a == L"--nosharedelete") shareDelete = false;
        else if (a == L"--dir") asDir = true;
        else {
            DWORD v = static_cast<DWORD>(_wtoi(a.c_str()));
            if (v > 0) seconds = v;
        }
    }

    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    if (shareDelete) share |= FILE_SHARE_DELETE;

    DWORD flags = asDir ? FILE_FLAG_BACKUP_SEMANTICS : 0;
    DWORD access = GENERIC_READ;

    HANDLE h = CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Print(L"打开失败 %s -> 0x%08lX", path.c_str(), GetLastError());
        return 2;
    }

    Print(L"已持有: %s", path.c_str());
    Print(L"共享模式: READ|WRITE%s", shareDelete ? L"|DELETE（可删）" : L"（不可删 -> 锁定）");
    Print(L"持有 %lu 秒，按 Ctrl+C 退出", seconds);

    Sleep(seconds * 1000);

    // 句柄在 CloseHandle / 进程退出时释放
    CloseHandle(h);
    Print(L"已释放");
    return 0;
}
