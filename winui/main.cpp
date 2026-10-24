#include "pch.h"

#include "App.xaml.h"
#include "InitialPaths.h"

#include "cli/CliMain.h"
#include "common/Lang.h"
#include "ipc/WorkerMain.h"
#include "ui/UiIpc.h"

using namespace winrt;

namespace {
std::vector<std::wstring> g_initialPaths;
}

namespace ul {

void SetInitialPaths(std::vector<std::wstring> paths) { g_initialPaths = std::move(paths); }

std::vector<std::wstring> TakeInitialPaths() {
    auto out = std::move(g_initialPaths);
    g_initialPaths.clear();
    return out;
}

}  // namespace ul

namespace {

bool HasFlag(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (auto const& a : args) if (_wcsicmp(a.c_str(), flag) == 0) return true;
    return false;
}

}  // namespace

// 同一个 exe 三种身份：--worker / CLI / WinUI。
// exe 必须仍然叫 Unlocker.exe —— worker 的三重校验要求「客户端映像路径 == 自身 exe」。
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::vector<std::wstring> args;
    if (argv) {
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    }

    if (HasFlag(args, L"--worker")) {
        const int rc = ul::WorkerMain(argc, argv);
        if (argv) LocalFree(argv);
        return rc;
    }

    if (ul::CliHasActionFlag(argc, argv)) {
        const int rc = ul::CliMain(argc, argv);
        if (argv) LocalFree(argv);
        return rc;
    }

    // 单实例按会话语义（Local\ 而不是 Global\）
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Unlocker.Instance");
    const bool alreadyRunning = mutex && GetLastError() == ERROR_ALREADY_EXISTS;

    if (alreadyRunning) {
        // 已有实例：把路径转发过去（内部带 3s 重试，覆盖首实例仍在 XAML 初始化的竞态）。
        // 转不出去时给一条可读提示，不要静默丢弃 —— 否则用户只会看到「窗口在，列表空」。
        if (!args.empty() && !ul::SendPathsToRunningInstance(args)) {
            MessageBoxW(nullptr,
                        L"Unlocker 已在运行，但这次传入的路径没能送过去。\n"
                        L"请关掉已打开的窗口后重试。",
                        L"Unlocker", MB_ICONINFORMATION | MB_OK);
        }
        if (mutex) CloseHandle(mutex);
        if (argv) LocalFree(argv);
        return 0;
    }

    ul::SetInitialPaths(args);
    if (argv) LocalFree(argv);

    // 语言：优先读用户上次的选择，没有就按系统首选 UI 语言判定（zh-* → 简中，其它 → 英文）
    ul::LoadLangPreference();

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    winrt::Unlocker::implementation::AppTrace(L"wWinMain: Application::Start 前");
    winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) {
        winrt::Unlocker::implementation::AppTrace(L"Start 回调: 创建 App");
        winrt::make<winrt::Unlocker::implementation::App>();
        winrt::Unlocker::implementation::AppTrace(L"Start 回调: App 创建完成");
    });
    winrt::Unlocker::implementation::AppTrace(L"wWinMain: Application::Start 返回");

    if (mutex) CloseHandle(mutex);
    return 0;
}
