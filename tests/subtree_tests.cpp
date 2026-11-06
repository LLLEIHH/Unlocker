// 子树占用断言集（对应 FS-RG22 误判那次回归）。
//
// 现象：目录里有文件被打开时，目录本身仍能用 DELETE 打开，于是旧逻辑判「可操作」，
// 但 MoveFile / RemoveDirectory 一律 ACCESS_DENIED —— 用户看到的是「扫不出占用却又改不了名」。
//
// 这里锁源必须来自**另一个进程**：HandleScanner 会主动跳过本进程及其父进程，
// 进程内自锁测不出任何东西。
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#include "common/Log.h"
#include "core/FileOps.h"
#include "core/HandleScanner.h"
#include "core/LockBreaker.h"
#include "core/PathResolver.h"
#include "core/PolicyGuard.h"

using namespace ul;

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const std::wstring& name, const std::wstring& detail = L"") {
    if (ok) {
        ++g_pass;
        wprintf(L"  [PASS] %s\n", name.c_str());
    } else {
        ++g_fail;
        wprintf(L"  [FAIL] %s   %s\n", name.c_str(), detail.c_str());
    }
}

std::wstring SelfDir() {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return {};
    std::wstring p(buf, n);
    const size_t pos = p.find_last_of(L'\\');
    return pos == std::wstring::npos ? std::wstring() : p.substr(0, pos);
}

bool EndsWithI(const std::wstring& s, const std::wstring& tail) {
    if (s.size() < tail.size()) return false;
    return _wcsicmp(s.c_str() + (s.size() - tail.size()), tail.c_str()) == 0;
}

// 起一个 MakeLock 子进程持有子目录句柄，模拟 Photoshop 那种「目录被打开」
struct LockProc {
    HANDLE proc = nullptr;
    DWORD  pid  = 0;

    bool Start(const std::wstring& target, const wchar_t* extra) {
        const std::wstring exe = SelfDir() + L"\\MakeLock.exe";
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            wprintf(L"  [SKIP] 找不到 MakeLock.exe，跳过锁源依赖用例\n");
            return false;
        }
        std::wstring cmd = L"\"" + exe + L"\" \"" + target + L"\" " + extra + L" 60";

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');
        if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            wprintf(L"  [FAIL] 启动 MakeLock 失败 %lu\n", GetLastError());
            return false;
        }
        CloseHandle(pi.hThread);
        proc = pi.hProcess;
        pid  = pi.dwProcessId;
        Sleep(400);   // 等它真的把句柄建起来
        return true;
    }

    ~LockProc() {
        if (proc) {
            TerminateProcess(proc, 0);
            WaitForSingleObject(proc, 3000);   // 等内核把句柄全部折掉，否则后续删除会撞残留
            CloseHandle(proc);
        }
    }
};

}  // namespace

int wmain() {
    PathResolver::Init();
    HandleScanner::Init(nullptr);
    std::wstring perr;
    if (!PolicyGuard::Init(&perr) || PolicyGuard::FailClosed()) {
        wprintf(L"[FAIL] 规则表自检失败，无法继续：%s\n", perr.c_str());
        return 1;
    }

    wprintf(L"== 子树占用 ==\n");

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"UnlockerSubtreeTests";
    const std::wstring parent  = root + L"\\Parent";
    const std::wstring parent2 = root + L"\\ParentRenamed";
    const std::wstring child   = parent + L"\\Child";
    const std::wstring childEx = parent + L"\\Child2";

    // 清理上一轮
    LockBreaker::CloseHandlesForPath(parent, 2, nullptr, nullptr, true);
    MoveFileExW(parent2.c_str(), parent.c_str(), 0);
    FileOps::Delete({parent}, ApplyOptions{}, nullptr);

    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(parent.c_str(), nullptr);
    CreateDirectoryW(child.c_str(), nullptr);
    CreateDirectoryW(childEx.c_str(), nullptr);
    {
        HANDLE h = CreateFileW((child + L"\\a.txt").c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    LockProc lock;
    if (lock.Start(child, L"--dir")) {
        // 1) 扫描父目录：必须报出子项占用，而不是「可操作」
        FileReport rep = HandleScanner::Scan(parent, ScanOptions{ false, false });
        Check(rep.subtreeLocked, L"父目录扫描识别出子树占用");
        Check(rep.state == OccupancyState::Locked, L"状态不得为 Free/Opened",
              L"实际=" + std::wstring(OccupancyText(rep.state)));
        Check(rep.descendantHandles >= 1, L"统计到子项句柄数",
              L"实际=" + std::to_wstring(rep.descendantHandles));

        bool sawVia = false;
        for (const auto& o : rep.owners) {
            if (!o.viaPath.empty() && o.proc.pid == lock.pid) {
                sawVia = EndsWithI(o.viaPath, L"\\Child") || EndsWithI(o.viaPath, L"Child");
            }
        }
        Check(sawVia, L"占用者标注了命中的子项路径");

        // 2) 子项锁着时，父目录改名必须失败（这就是用户看到的「无法重命名」）
        const std::wstring before = parent;
        BOOL moved = MoveFileExW(before.c_str(), parent2.c_str(), 0);
        const DWORD moveErr = moved ? ERROR_SUCCESS : GetLastError();
        Check(!moved && moveErr == ERROR_ACCESS_DENIED,
              L"子项占用时父目录改名被拒（复现原始故障）",
              L"err=" + std::to_wstring(moveErr));
        if (moved) MoveFileExW(parent2.c_str(), before.c_str(), 0);

        // 3) 走 FileOps::Rename：应当自己松开子树句柄并改名成功
        BatchResult br = FileOps::Rename(parent, L"ParentRenamed", ApplyOptions{}, nullptr);
        Check(br.okCount == 1, L"FileOps::Rename 解掉子树锁后改名成功",
              br.items.empty() ? L"" : br.items.front().message);
        Check(GetFileAttributesW(parent2.c_str()) != INVALID_FILE_ATTRIBUTES,
              L"改名结果落盘");
    }

    // 收尾
    if (GetFileAttributesW(parent2.c_str()) != INVALID_FILE_ATTRIBUTES) {
        FileOps::Delete({parent2}, ApplyOptions{}, nullptr);
    }
    FileOps::Delete({parent}, ApplyOptions{}, nullptr);

    wprintf(L"\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
