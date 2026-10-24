#pragma once

#include "MainWindow.g.h"
#include "MainWindow.xaml.g.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "core/HandleScanner.h"
#include "core/PolicyGuard.h"
#include "ipc/PipeClient.h"

namespace winrt::Unlocker::implementation {

struct UiOwner {
    uint32_t pid = 0;
    std::wstring name;
    std::wstring desc;
    std::wstring fullPath;
    int handles = 1;
    bool prot = false;
    bool image = false;
};

struct UiItem {
    uint64_t id = 0;
    std::wstring path;
    std::wstring canonical;
    ul::OccupancyState state = ul::OccupancyState::Free;
    uint32_t error = 0;
    unsigned long long size = 0;
    bool dir = false;
    bool scanned = false;
    bool scanning = false;
    bool needElevation = false;
    int handles = 0;
    std::vector<UiOwner> owners;
    std::vector<DWORD> skipped;
    ul::Verdict verdict = ul::Verdict::Allow;
    std::wstring rule;
    std::wstring reason;
};

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    ~MainWindow();

    // XAML 事件（签名必须与 .xaml 中的绑定一致）
    void OnDragOver(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::DragEventArgs const&);
    winrt::fire_and_forget OnDrop(winrt::Windows::Foundation::IInspectable const&,
                                  winrt::Microsoft::UI::Xaml::DragEventArgs const&);
    void OnAddFiles(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAddFolders(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnDeepScan(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnUnlock(winrt::Windows::Foundation::IInspectable const&,
                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnDelete(winrt::Windows::Foundation::IInspectable const&,
                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnRename(winrt::Windows::Foundation::IInspectable const&,
                                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnMove(winrt::Windows::Foundation::IInspectable const&,
                winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnCopy(winrt::Windows::Foundation::IInspectable const&,
                winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnRescan(winrt::Windows::Foundation::IInspectable const&,
                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnFileSelectionChanged(winrt::Windows::Foundation::IInspectable const&,
                                winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

    // 键盘加速键（F5 / Ctrl+O / Ctrl+A / Del / Ctrl+Del / Enter）
    void OnSelectAll(winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                     winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const&);
    void OnUnlockAccelerator(winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                             winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const&);
    void OnDeleteAccelerator(winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                             winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const&);
    void OnRemoveFromListAccelerator(
        winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
        winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const&);

    // 给单实例转发 / 右键菜单唤醒用
    void AddPaths(std::vector<std::wstring> const& paths);
    HWND Hwnd();

    // 简单的信息提示（ContentDialog 必须在 UI 线程且设置 XamlRoot）
    void ShowMessage(std::wstring const& title, std::wstring const& text);

private:
    void SetupWindow();
    void ApplyNarrowLayout();
    // 按当前语言（ul::GetLang()）把静态文案、提示、横幅、列表全部刷一遍。
    // 切换语言后调用它即可，不需要重启。
    void ApplyLanguage();
    void RebuildFileList();
    void RebuildProcessList();
    void SetStatus(std::wstring const& text);
    void SetBusy(bool busy);
    void UpdateSummary();
    void StartLocalScan(std::vector<std::pair<uint64_t, std::wstring>> targets);
    void StartDeepScan(std::vector<std::pair<uint64_t, std::wstring>> targets);
    void StartOp(ul::IpcOp op, std::wstring arg);
    std::vector<size_t> SelectedIndices();
    void RefreshEmptyHint();

    std::vector<UiItem> items_;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::Grid> rows_;   // 与 items_ 一一对应，用于选中映射
    uint64_t nextId_ = 1;
    bool elevated_ = false;
    bool failClosed_ = false;
    bool busy_ = false;

    std::atomic<bool> alive_{ true };
    ul::PipeClient worker_;
    std::mutex workerMutex_;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_{ nullptr };
};

}  // namespace winrt::Unlocker::implementation

namespace winrt::Unlocker::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}  // namespace winrt::Unlocker::factory_implementation
