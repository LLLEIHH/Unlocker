#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "XamlTypeLookup.h"

// XAML 编译器把 InitializeComponent/Connect 的实现放在 .xaml.g.hpp 里
#include "App.xaml.g.hpp"

#include <cstdio>
#include <string>

#include <restrictederrorinfo.h>  // IRestrictedErrorInfo（显式包含，勿依赖 windows.h 包含链）
#include <oleauto.h>  // SysStringLen / SysFreeString（WIN32_LEAN_AND_MEAN 下 windows.h 不自带）

#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace winrt::Unlocker::implementation {

// 启动/操作分步追踪开关。M5.1 排障期是「永远写」，会持续向
// %LOCALAPPDATA%\Unlocker\winui-crash.log 追加；现在默认只在 Debug 构建打开，
// Release 下 AppTrace 编译成空操作（未处理异常记录不受影响，仍然总会写）。
#if !defined(UNLOCKER_WINUI_TRACE)
#if defined(_DEBUG)
#define UNLOCKER_WINUI_TRACE 1
#else
#define UNLOCKER_WINUI_TRACE 0
#endif
#endif

namespace {

// 把未处理异常写到本地日志（fail-fast 之前最后一次留证的机会）
void WriteCrashLog(std::wstring const& line) {
    wchar_t path[MAX_PATH * 2]{};
    if (!ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Unlocker\\winui-crash.log", path, ARRAYSIZE(path))) return;
    if (FILE* f = nullptr; _wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f) {
        fwrite(line.data(), 1, line.size() * sizeof(wchar_t), f);
        fclose(f);
    }
}

}  // namespace

void AppTrace(std::wstring const& step) {
#if UNLOCKER_WINUI_TRACE
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t head[80]{};
    swprintf_s(head, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    WriteCrashLog(std::wstring(head) + step + L"\n");
#else
    UNREFERENCED_PARAMETER(step);
#endif
}

App::App() {
    AppTrace(L"App ctor: 开始");
    InitializeComponent();
    AppTrace(L"App ctor: InitializeComponent 完成");

    UnhandledException([this](IInspectable const&, UnhandledExceptionEventArgs const& e) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t head[64]{};
        swprintf_s(head, L"[%04u-%02u-%02u %02u:%02u:%02u] UNHANDLED hr=0x%08X\n",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                   static_cast<uint32_t>(e.Exception()));
        WriteCrashLog(head);
        const std::wstring msg = std::wstring(e.Message().c_str()) + L"\n";

        // 线程上可能还挂着 IRestrictedErrorInfo（含 originate 时的原始描述）。
        // 用裸 COM 指针：winrt::com_ptr<T> 对该接口的 CTAD 在本环境不可靠。
        ::IErrorInfo* err = nullptr;
        if (SUCCEEDED(::GetErrorInfo(0, &err)) && err) {
            ::IRestrictedErrorInfo* info = nullptr;
            if (SUCCEEDED(err->QueryInterface(IID_PPV_ARGS(&info))) && info) {
                ::BSTR raw = nullptr;
                ::BSTR restricted = nullptr;
                ::BSTR capSid = nullptr;
                HRESULT origHr = S_OK;
                if (SUCCEEDED(info->GetErrorDetails(&raw, &origHr, &restricted, &capSid)) && raw) {
                    wchar_t hrTxt[16]{};
                    swprintf_s(hrTxt, L"0x%08X ", static_cast<uint32_t>(origHr));
                    WriteCrashLog(L"[restricted] hr=" + std::wstring(hrTxt) +
                                  std::wstring(raw, ::SysStringLen(raw)) + L"\n");
                }
                if (raw) ::SysFreeString(raw);
                if (restricted) ::SysFreeString(restricted);
                if (capSid) ::SysFreeString(capSid);
                info->Release();
            }
            err->Release();
        }
        WriteCrashLog(msg + L"----\n");
    });
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    AppTrace(L"OnLaunched: 创建 MainWindow");

    // App.xaml 已在 Application.Resources 里合并 XamlControlsResources（框架控件默认样式）。
    // 它能加载的前提是 GetXamlType 委托到生成的 provider —— 见文件末尾的实现。

    try {
        window_ = winrt::make<MainWindow>();
        AppTrace(L"OnLaunched: MainWindow 构造完成");
    } catch (winrt::hresult_error const& e) {
        AppTrace(L"OnLaunched: MainWindow 构造抛出 hr=0x" + std::to_wstring(static_cast<uint32_t>(e.code())) +
                 L" msg=" + std::wstring(e.message().c_str()));
        throw;
    }
    try {
        window_.Activate();
        AppTrace(L"OnLaunched: Activate 完成");
    } catch (winrt::hresult_error const& e) {
        AppTrace(L"OnLaunched: Activate 抛出 hr=0x" + std::to_wstring(static_cast<uint32_t>(e.code())) +
                 L" msg=" + std::wstring(e.message().c_str()));
        throw;
    }
}

// ------------------------------------------------------------ IXamlMetadataProvider
// 委托给 XAML 编译器生成的 provider（见 XamlTypeLookup.h 的说明）。
// 生成的 provider 认得本工程的 App/MainWindow，并会回落到框架的
// XamlControlsXamlMetaDataProvider —— 这是 XamlControlsResources 能被实例化的前提。

winrt::Microsoft::UI::Xaml::Markup::IXamlType App::GetXamlType(
    winrt::Windows::UI::Xaml::Interop::TypeName const& type) {
    try {
        return ul::LookupXamlType(type);
    } catch (winrt::hresult_error const& e) {
        AppTrace(L"GetXamlType(TypeName) 失败 hr=0x" +
                 std::to_wstring(static_cast<uint32_t>(e.code())));
        return nullptr;
    }
}

winrt::Microsoft::UI::Xaml::Markup::IXamlType App::GetXamlType(winrt::hstring const& fullName) {
    try {
        return ul::LookupXamlType(fullName);
    } catch (winrt::hresult_error const& e) {
        AppTrace(L"GetXamlType(" + std::wstring(fullName.c_str()) + L") 失败 hr=0x" +
                 std::to_wstring(static_cast<uint32_t>(e.code())));
        return nullptr;
    }
}

winrt::com_array<winrt::Microsoft::UI::Xaml::Markup::XmlnsDefinition> App::GetXmlnsDefinitions() {
    try {
        return ul::LookupXmlnsDefinitions();
    } catch (winrt::hresult_error const&) {
        return winrt::com_array<winrt::Microsoft::UI::Xaml::Markup::XmlnsDefinition>(0);
    }
}

}  // namespace winrt::Unlocker::implementation
