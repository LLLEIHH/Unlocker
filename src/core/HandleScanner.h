#pragma once
// 句柄扫描器（方案 4.2）——整个项目的技术核心。
//
// 分两级（F20）：L1 在 UI 进程内免提权跑，覆盖同 IL 进程；深扫（L2）在 worker 里跑。
// 枚举本身不需要管理员，需要 SeDebugPrivilege 的只有 OpenProcess 打开高 IL/PPL 进程。
#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "common/ErrorMap.h"

namespace ul {

struct ProcessInfo {
    DWORD        pid = 0;
    std::wstring name;          // notepad.exe
    std::wstring fullPath;      // QueryFullProcessImageNameW
    std::wstring description;   // 版本资源 FileDescription（语言块回退）
    std::wstring company;
    bool         elevated      = false;
    bool         protectedProc = false;   // OpenProcess 失败
    int          handleCount   = 0;       // 该进程对目标文件持有的句柄数
};

struct HandleOwner {
    ProcessInfo proc;
    HANDLE      handleValue   = nullptr;
    ULONG       grantedAccess = 0;
    bool        isImageMapping = false;
};

struct FileReport {
    std::wstring              path;       // 用户传入
    std::wstring              canonical;  // 归一化真身
    OccupancyState            state = OccupancyState::Free;
    DWORD                     error = 0;
    unsigned long long        size  = 0;
    bool                      directory = false;
    bool                      needElevation = false;   // L1 遇到 AccessDenied / 高 IL 进程
    int                       totalFileHandles = 0;
    std::vector<HandleOwner>  owners;
    std::vector<DWORD>        skippedPids;             // OpenProcess 失败且持有文件句柄的进程
};

struct ScanOptions {
    bool elevated          = false;   // 当前进程是否高完整性（决定是否提示深扫）
    bool detectImageMapping = true;
};

using ProgressFn = std::function<void(const std::wstring& stage, int cur, int total)>;

class HandleScanner {
public:
    static bool Init(std::wstring* err);
    static bool Ready();

    // 打开-枚举顺序不能反：先 Probe 再枚举，否则「只是被打开、其实能删」会被误报成锁定
    static bool Probe(const std::wstring& path, OccupancyState* state, DWORD* win32Error);

    static FileReport Scan(const std::wstring& path, const ScanOptions& opts,
                           const ProgressFn& progress = nullptr,
                           std::atomic<bool>* cancel = nullptr);

    // 只取占用者（关句柄用），不做状态判定
    static std::vector<HandleOwner> FindOwners(const std::wstring& path,
                                               std::vector<DWORD>* skippedPids,
                                               std::atomic<bool>* cancel = nullptr);

    static bool IsImageMappedBy(const std::wstring& canonical,
                                const std::vector<DWORD>& pids);

    static int FileTypeIndex();
};

}  // namespace ul
