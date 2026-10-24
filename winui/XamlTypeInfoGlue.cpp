// 生成的 XamlTypeInfo.g.cpp 会直接引用 implementation::App / implementation::MainWindow，
// 但它自己只包含 pch.h 和 winrt 头。这里先把工程类的声明带进来，再包含生成实现，
// 等价于 VS 的 C++ XAML 胶水所做的“把生成文件编进工程”这件事。
#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "XamlTypeLookup.h"

#include "XamlTypeInfo.g.cpp"

namespace ul {

namespace {

// 生成的 provider 既认得本工程的 App/MainWindow，也会回落到框架 provider
// （XamlTypeInfo.g.cpp 的 OtherProviders() 里 push 了 XamlControlsXamlMetaDataProvider）。
// 全进程共用一个实例：它内部有类型缓存（std::map<wstring, weak_ref<IXamlType>>）。
std::shared_ptr<winrt::Unlocker::implementation::XamlTypeInfoProvider> Provider() {
    static const auto instance =
        std::make_shared<winrt::Unlocker::implementation::XamlTypeInfoProvider>();
    return instance;
}

}  // namespace

winrt::Microsoft::UI::Xaml::Markup::IXamlType LookupXamlType(winrt::hstring const& fullName) {
    return Provider()->GetXamlTypeByName(fullName);
}

winrt::Microsoft::UI::Xaml::Markup::IXamlType LookupXamlType(
    winrt::Windows::UI::Xaml::Interop::TypeName const& type) {
    return Provider()->GetXamlTypeByType(type);
}

winrt::com_array<winrt::Microsoft::UI::Xaml::Markup::XmlnsDefinition> LookupXmlnsDefinitions() {
    return winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider()
        .GetXmlnsDefinitions();
}

}  // namespace ul
