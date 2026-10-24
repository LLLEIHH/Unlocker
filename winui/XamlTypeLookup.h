#pragma once

// XAML 元数据查询入口。
//
// 背景（M5.1 根因）：XAML 编译器会为工程生成一份完整的元数据提供器
// （`XamlTypeInfo.g.cpp` 里的 `winrt::Unlocker::implementation::XamlTypeInfoProvider`），
// 它既能解析本工程的 App/MainWindow，也会回落到框架自己的
// `XamlControlsXamlMetaDataProvider`（见生成文件里 `OtherProviders()`）。
//
// 但 `XamlMetaDataProvider.cpp` 不在 vcxproj 的编译列表里，App 也从未引用生成类，
// 于是 App 的 IXamlMetadataProvider 只能对一切类型返回 nullptr —— 框架在实例化
// `XamlControlsResources`（App.xaml 合并字典）时拿不到元数据，直接 fail-fast（0x802B000A）。
//
// 这里把生成类暴露成三个自由函数，由 XamlTypeInfoGlue.cpp（唯一包含 .g.cpp 的 TU）实现，
// App 的三个 IXamlMetadataProvider 方法转调过来。不改手工投影头，也不引入 x:Bind。

#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

namespace ul {

winrt::Microsoft::UI::Xaml::Markup::IXamlType LookupXamlType(winrt::hstring const& fullName);
winrt::Microsoft::UI::Xaml::Markup::IXamlType LookupXamlType(
    winrt::Windows::UI::Xaml::Interop::TypeName const& type);

// 本工程 XAML 没有自定义 xmlns 定义，直接采用框架 provider 的映射
// （App.xaml 里的 xmlns="using:Microsoft.UI.Xaml.Controls" 依赖它）。
winrt::com_array<winrt::Microsoft::UI::Xaml::Markup::XmlnsDefinition> LookupXmlnsDefinitions();

}  // namespace ul
