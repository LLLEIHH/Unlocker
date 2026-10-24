#pragma once
// 文件操作（方案 4.5）：删除 / 改名 / 移动 / 复制。
// 所有操作内部都会先过 PolicyGuard，再按 Level 0/1/2/4/5 逐级尝试。
//
// 递归删除与黑名单的交互（必须定死，否则各处实现不一致）：
//   目录本身 Deny        -> 整棵子树不做任何删除，只报告
//   子项 Deny            -> 跳过该子项，其余照常删，结束时汇总
//   子项 Confirm         -> 一次性列出，由调用方（worker）弹一次框，确认后对整批生效
//   普通失败             -> 记录并继续，最后统一汇报
//   先遍历预检、再统一执行，不做「边遍历边删」——否则确认框就成了通知。
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace ul {

struct ApplyOptions {
    bool         force        = false;   // --force：只允许杀进程，不等于无视黑名单
    bool         allowReboot  = false;   // --reboot：失败时排队到重启
    bool         quiet        = false;   // --quiet：不弹任何框（含 Confirm）
    bool         elevated     = false;   // 当前进程是否高完整性
    std::wstring caller       = L"GUI";  // GUI / CLI / Shell，写审计日志用
};

struct ItemResult {
    std::wstring path;
    bool         ok             = false;
    bool         skipped        = false;
    bool         rebootRequired = false;
    DWORD        code           = 0;     // Win32 错误码；语义码见 ExitCode
    int          level          = -1;    // 实际用到的策略层级
    std::wstring rule;                   // 命中规则（BL-xxx）
    std::wstring message;
    std::vector<std::wstring> owners;    // 涉及/已终止的进程
    int          closedHandles  = 0;
};

struct ConfirmRequest {
    std::vector<std::wstring> paths;
    std::wstring              rule;
    std::wstring              reason;
    bool                      batch = false;
    std::wstring              op;
};

using ConfirmFn = std::function<bool(const ConfirmRequest&)>;

struct BatchResult {
    std::vector<ItemResult> items;
    int  okCount      = 0;
    int  failCount    = 0;
    int  skipCount    = 0;
    bool needConfirm  = false;   // quiet 模式下遇到 Confirm 项
    bool anyReboot    = false;
    bool cancelled    = false;

    bool AllOk() const { return failCount == 0 && skipCount == 0; }
};

class FileOps {
public:
    static BatchResult Unlock(const std::vector<std::wstring>& paths, const ApplyOptions& opts,
                              const ConfirmFn& confirm);
    static BatchResult Delete(const std::vector<std::wstring>& paths, const ApplyOptions& opts,
                              const ConfirmFn& confirm);
    static BatchResult Rename(const std::wstring& path, const std::wstring& newName,
                              const ApplyOptions& opts, const ConfirmFn& confirm);
    static BatchResult Move(const std::vector<std::wstring>& paths, const std::wstring& destDir,
                            const ApplyOptions& opts, const ConfirmFn& confirm);
    static BatchResult Copy(const std::vector<std::wstring>& paths, const std::wstring& destDir,
                            const ApplyOptions& opts, const ConfirmFn& confirm);
};

}  // namespace ul
