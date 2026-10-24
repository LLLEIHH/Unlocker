#include "ipc/WorkerMain.h"

#include <string>

#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/HandleScanner.h"
#include "core/PathResolver.h"
#include "core/PolicyGuard.h"
#include "core/Privilege.h"
#include "ipc/PipeServer.h"

namespace ul {
namespace {

std::wstring ArgValue(const std::wstring& arg, const wchar_t* key) {
    const size_t klen = wcslen(key);
    if (arg.size() > klen && _wcsnicmp(arg.c_str(), key, klen) == 0 && arg[klen] == L'=') {
        return arg.substr(klen + 1);
    }
    return {};
}

}  // namespace

int WorkerMain(int argc, wchar_t** argv) {
    LogInit(L"Unlocker");

    WorkerContext ctx;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        // 三个分支各用独立变量名：同一作用域链里重复声明同名变量会触发 C4456
        if (const std::wstring pipeArg = ArgValue(arg, L"--pipe"); !pipeArg.empty()) {
            ctx.pipeName = pipeArg;
        } else if (const std::wstring pidArg = ArgValue(arg, L"--parent-pid"); !pidArg.empty()) {
            ctx.expectedUiPid = static_cast<DWORD>(_wtoi(pidArg.c_str()));
        } else if (const std::wstring sidArg = ArgValue(arg, L"--ui-sid"); !sidArg.empty()) {
            ctx.uiUserSid = sidArg;
        } else if (arg == L"--quiet") {
            ctx.quiet = true;
        }
    }

    // 裸跑 worker：不带 pipe 参数时立即退出，不产生任何行为（T38）
    if (ctx.pipeName.empty() || ctx.expectedUiPid == 0) {
        LogE(L"worker 启动参数缺失，拒绝运行");
        return 2;
    }

    LogI(std::wstring(L"worker 启动，pipe=") + MaskSecret(ctx.pipeName) +
         L" parent=" + std::to_wstring(ctx.expectedUiPid));

    PathResolver::Init();
    std::wstring err;
    if (!PolicyGuard::Init(&err)) {
        // fail-closed：即使规则表自检失败也不退出，让每次裁决都返回 Deny
        LogE(L"PolicyGuard 初始化异常（已 fail-closed）：" + err);
    }
    if (!HandleScanner::Init(&err)) {
        LogE(L"HandleScanner 初始化失败：" + err);
    }
    const PrivilegeReport priv = EnableWorkerPrivileges();
    if (!priv.restore) {
        // DELETE/写访问的 ACL 豁免靠 SeRestorePrivilege，缺了它会出现「看起来提权了、还是打不开」
        LogW(L"SeRestorePrivilege 未启用，部分 ACL 拒绝的文件仍将无法操作");
    }

    return PipeServer::Run(ctx);
}

}  // namespace ul
