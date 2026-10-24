#pragma once

#include "App.g.h"
#include "App.xaml.g.h"

namespace winrt::Unlocker::implementation {

// 启动期步骤追踪，写到 %LOCALAPPDATA%\Unlocker\winui-crash.log
void AppTrace(std::wstring const& step);

// App 必须实现 IXamlMetadataProvider（WASDK 1.7 起接口名没有大写 D，旧名
// IXamlMetaDataProvider 已从 winmd 移除）：XAML 框架在启动早期会对 Application
// 对象做 QI，取不到该接口时直接 fail-fast（0x802B000A）。
// 接口通过 UnlockerApp.idl 声明进 App 的投影基类。
//
// 三个方法实现为**委托给 XAML 编译器生成的 provider**（XamlTypeLookup.h）：
// 生成的 XamlTypeInfoProvider 认得本工程的 App/MainWindow，并会回落到框架的
// XamlControlsXamlMetaDataProvider。早期版本对一切类型返回 nullptr，导致框架实例化
// XamlControlsResources 时拿不到元数据 → 启动期 fail-fast（M5.1 的真正根因）。
struct App : AppT<App> {
    App();

    void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

    // IXamlMetadataProvider
    winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type);
    winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(winrt::hstring const& fullName);
    winrt::com_array<winrt::Microsoft::UI::Xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions();

private:
    winrt::Microsoft::UI::Xaml::Window window_{ nullptr };
};

}  // namespace winrt::Unlocker::implementation

namespace winrt::Unlocker::factory_implementation {
struct App : AppT<App, implementation::App> {};
}  // namespace winrt::Unlocker::factory_implementation
