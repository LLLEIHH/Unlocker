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
    // 非空 = 命中的是子项（目录目标的子树扫描）。重命名/删除目录时，只要子树里有
    // 任何一个对象被打开，MoveFile / RemoveDirectory 都会返回 ACCESS_DENIED，
    // 光看目录自身的 DELETE 权限会漏判——见 Scan 里的 subtreeLocked。
    std::wstring viaPath;
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
    bool                      subtreeLocked = false;   // 目录自身可操作，但子树内有占用（改名/删除会被拒）
    int                       descendantHandles = 0;   // 命中子项的句柄数（owners 里 viaPath 非空的个数）
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

    // 只取占用者（关句柄用），不做状态判定。
    // includeDescendants=true 时命中也算：「路径等于目标」或「路径以 目标\ 开头」，
    // 命中子项的 owner.viaPath 会填成该子项的完整路径。
    static std::vector<HandleOwner> FindOwners(const std::wstring& path,
                                               std::vector<DWORD>* skippedPids,
                                               std::atomic<bool>* cancel = nullptr,
                                               bool includeDescendants = false);

    static bool IsImageMappedBy(const std::wstring& canonical,
                                const std::vector<DWORD>& pids);

    static int FileTypeIndex();
};

}  // namespace ul
