// 同一个 exe 三种身份：
//   --worker  内部协议（不对用户公开，必须带 --pipe 与 --parent-pid）
//   有动作参数  CLI
//   其余       GUI
//
// 重要：manifest 里的 asInvoker 是功能依赖，不是风格选择。
// 一旦改成 requireAdministrator，拖放会在 Win10/11 上直接静默失效（收不到 WM_DROPFILES），
// 不报错、不崩溃，排查成本极高。UI 必须留在中完整性，权限属于「动作」，不属于「窗口」。
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "cli/CliMain.h"
#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/PathResolver.h"
#include "ipc/WorkerMain.h"
#include "ui/MainWindow.h"
#include "ui/UiIpc.h"

namespace {

std::vector<std::wstring> SplitArgs() {
    int argc = 0;
    std::vector<std::wstring> out;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return out;
    for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
    LocalFree(argv);
    return out;
}

bool HasFlag(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (const auto& a : args) if (_wcsicmp(a.c_str(), flag) == 0) return true;
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    // 控制台输出（CLI 场景）在 AttachConsole 之后才可用
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    if (HasFlag(std::vector<std::wstring>(argv + 1, argv + argc), L"--worker")) {
        const int rc = ul::WorkerMain(argc, argv);
        LocalFree(argv);
        return rc;
    }

    if (ul::CliHasActionFlag(argc, argv)) {
        const int rc = ul::CliMain(argc, argv);
        LocalFree(argv);
        return rc;
    }

    // GUI：单实例按会话语义（Local\ 而不是 Global\）
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Unlocker.Instance");
    std::vector<std::wstring> paths(argv + 1, argv + argc);
    LocalFree(argv);

    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!paths.empty()) ul::SendPathsToRunningInstance(paths);
        CloseHandle(mutex);
        return 0;
    }

    const int rc = ul::GuiMain(hInst, paths, nCmdShow);
    if (mutex) CloseHandle(mutex);
    return rc;
}
