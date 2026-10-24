#include "pch.h"
#include "MainWindow.xaml.h"
#include "InitialPaths.h"
#include "App.xaml.h"

// XAML 编译器把 InitializeComponent/Connect 的实现放在 .xaml.g.hpp 里
#include "MainWindow.xaml.g.hpp"

#include <shlobj.h>

#include <algorithm>
#include <set>
#include <utility>

#include "common/ErrorMap.h"
#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/FileOps.h"
#include "core/PathResolver.h"
#include "core/Privilege.h"
#include "ipc/Protocol.h"
#include "ui/ElevationCheck.h"
#include "ui/FilePicker.h"
#include "ui/UiIpc.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator;
using winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs;

namespace winrt::Unlocker::implementation {
namespace {

hstring Hs(std::wstring const& s) { return hstring(s); }

std::wstring Ws(hstring const& s) { return std::wstring(s.c_str()); }

bool IsDark(ElementTheme theme) { return theme == ElementTheme::Dark; }

Windows::UI::Color Rgb(uint8_t r, uint8_t g, uint8_t b) {
    return Windows::UI::ColorHelper::FromArgb(255, r, g, b);
}

SolidColorBrush TextPrimary(bool dark)  { return SolidColorBrush(dark ? Rgb(0xF0, 0xF0, 0xF0) : Rgb(0x1A, 0x1A, 0x1A)); }
SolidColorBrush TextSecondary(bool dark) { return SolidColorBrush(dark ? Rgb(0xA0, 0xA0, 0xA0) : Rgb(0x6B, 0x6B, 0x6B)); }

// 状态配色：深色模式下红/橙要抬亮度，否则在亚克力上读不清
SolidColorBrush StateBrush(ul::OccupancyState s, bool denied, bool dark) {
    if (denied) return SolidColorBrush(dark ? Rgb(0xFF, 0x99, 0x99) : Rgb(0xB0, 0x00, 0x00));
    switch (s) {
        case ul::OccupancyState::Locked:         return SolidColorBrush(dark ? Rgb(0xFF, 0x8A, 0x8A) : Rgb(0xC0, 0x00, 0x00));
        case ul::OccupancyState::ImageMapped:    return SolidColorBrush(dark ? Rgb(0xFF, 0xB8, 0x6B) : Rgb(0xC8, 0x60, 0x00));
        case ul::OccupancyState::AccessDenied:   return SolidColorBrush(dark ? Rgb(0xF5, 0xD0, 0x6B) : Rgb(0x96, 0x6E, 0x00));
        case ul::OccupancyState::Opened:         return SolidColorBrush(dark ? Rgb(0xB0, 0xB0, 0xB0) : Rgb(0x6E, 0x6E, 0x6E));
        case ul::OccupancyState::Free:           return SolidColorBrush(dark ? Rgb(0x6C, 0xD4, 0x6C) : Rgb(0x0F, 0x7B, 0x0F));
        case ul::OccupancyState::LockedByKernel: return SolidColorBrush(dark ? Rgb(0xC0, 0xA0, 0xE0) : Rgb(0x78, 0x46, 0x9C));
        case ul::OccupancyState::NotFound:       return SolidColorBrush(dark ? Rgb(0x9A, 0x9A, 0x9A) : Rgb(0x82, 0x82, 0x82));
        case ul::OccupancyState::Error:          return SolidColorBrush(dark ? Rgb(0x9A, 0x9A, 0x9A) : Rgb(0x5A, 0x5A, 0x5A));
    }
    return TextPrimary(dark);
}

std::wstring FileName(std::wstring const& path) {
    const size_t pos = path.find_last_of(L'\\');
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring OwnersSummary(UiItem const& it) {
    if (it.owners.empty()) return L"—";
    std::wstring first = it.owners.front().name;
    if (first.empty()) first = L"PID " + std::to_wstring(it.owners.front().pid);
    if (it.owners.size() > 1) first += L" (+" + std::to_wstring(it.owners.size() - 1) + L")";
    return first;
}

std::wstring StateText(UiItem const& it) {
    if (it.verdict == ul::Verdict::Deny) return L"✕ 已阻止";
    if (!it.scanned) return it.scanning ? L"扫描中…" : L"待扫描";
    std::wstring t = ul::OccupancyText(it.state);
    if (it.verdict == ul::Verdict::Confirm) t = std::wstring(L"需确认 · ") + t;
    return t;
}

TextBlock MakeText(hstring text, double size, Brush const& brush, bool ellipsis) {
    TextBlock tb;
    tb.Text(text);
    tb.FontSize(size);
    tb.Foreground(brush);
    tb.VerticalAlignment(VerticalAlignment::Center);
    tb.TextWrapping(TextWrapping::NoWrap);
    if (ellipsis) tb.TextTrimming(TextTrimming::CharacterEllipsis);
    return tb;
}

ColumnDefinition StarCol() {
    ColumnDefinition cd;
    cd.Width(GridLength(1, GridUnitType::Star));
    return cd;
}

ColumnDefinition FixedCol(double px) {
    ColumnDefinition cd;
    cd.Width(GridLength(px, GridUnitType::Pixel));
    return cd;
}

// ------------------------------------------------------------ 现代化视觉组件

// 徽章底/前景配色（深浅模式各自调过亮度，保证亚克力上可读）
struct Tone { Windows::UI::Color bg, fg; };

Tone ToneFor(UiItem const& it, bool dark) {
    if (it.verdict == ul::Verdict::Deny) {
        // 已阻止：实心红底白字，视觉上最重
        return dark ? Tone{ Rgb(0x6B, 0x1D, 0x1D), Rgb(0xFF, 0xC5, 0xC5) }
                    : Tone{ Rgb(0xA3, 0x2D, 0x2D), Rgb(0xFF, 0xFF, 0xFF) };
    }
    if (!it.scanned) {
        return dark ? Tone{ Rgb(0x3A, 0x3A, 0x38), Rgb(0xC8, 0xC8, 0xC8) }
                    : Tone{ Rgb(0xF1, 0xEF, 0xE8), Rgb(0x5F, 0x5E, 0x5A) };
    }
    switch (it.state) {
        case ul::OccupancyState::Locked:
            return dark ? Tone{ Rgb(0x4E, 0x14, 0x14), Rgb(0xFF, 0x9E, 0x9E) }
                        : Tone{ Rgb(0xFC, 0xEB, 0xEB), Rgb(0xA3, 0x2D, 0x2D) };
        case ul::OccupancyState::ImageMapped:
            return dark ? Tone{ Rgb(0x45, 0x2C, 0x06), Rgb(0xFF, 0xBC, 0x74) }
                        : Tone{ Rgb(0xFA, 0xEE, 0xDA), Rgb(0x85, 0x4F, 0x0B) };
        case ul::OccupancyState::AccessDenied:
            return dark ? Tone{ Rgb(0x40, 0x33, 0x08), Rgb(0xFF, 0xDF, 0x8A) }
                        : Tone{ Rgb(0xFC, 0xEF, 0xCF), Rgb(0x7A, 0x54, 0x08) };
        case ul::OccupancyState::Free:
            return dark ? Tone{ Rgb(0x1E, 0x38, 0x0B), Rgb(0x9E, 0xCC, 0x60) }
                        : Tone{ Rgb(0xEA, 0xF3, 0xDE), Rgb(0x3B, 0x6D, 0x11) };
        case ul::OccupancyState::Opened:
            return dark ? Tone{ Rgb(0x3A, 0x3A, 0x38), Rgb(0xC8, 0xC8, 0xC8) }
                        : Tone{ Rgb(0xF1, 0xEF, 0xE8), Rgb(0x5F, 0x5E, 0x5A) };
        case ul::OccupancyState::LockedByKernel:
            return dark ? Tone{ Rgb(0x33, 0x24, 0x4A), Rgb(0xD0, 0xBC, 0xF0) }
                        : Tone{ Rgb(0xEF, 0xEB, 0xFA), Rgb(0x5B, 0x3C, 0x8C) };
        case ul::OccupancyState::NotFound:
        case ul::OccupancyState::Error:
        default:
            return dark ? Tone{ Rgb(0x2E, 0x2E, 0x2C), Rgb(0x9A, 0x9A, 0x9A) }
                        : Tone{ Rgb(0xF1, 0xEF, 0xE8), Rgb(0x88, 0x87, 0x80) };
    }
}

// 彩色圆角状态徽章
Border MakeBadge(hstring const& text, Tone tone) {
    Border b;
    b.CornerRadius(CornerRadius{ 99, 99, 99, 99 });
    b.Padding(Thickness{ 10, 2, 10, 3 });
    b.Background(SolidColorBrush(tone.bg));
    b.VerticalAlignment(VerticalAlignment::Center);
    b.HorizontalAlignment(HorizontalAlignment::Left);
    auto tb = MakeText(text, 12, SolidColorBrush(tone.fg), true);
    b.Child(tb);
    return b;
}

// 文件类型色块（扩展名分组着色）
Tone TileTone(std::wstring const& path, bool dir, bool dark) {
    if (dir) {
        return dark ? Tone{ Rgb(0x0C, 0x30, 0x55), Rgb(0x85, 0xB7, 0xEB) }
                    : Tone{ Rgb(0xE6, 0xF1, 0xFB), Rgb(0x18, 0x5F, 0xA5) };
    }
    const size_t dot = path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : path.substr(dot + 1);
    for (auto& c : ext) c = static_cast<wchar_t>(towupper(c));
    if (ext == L"EXE" || ext == L"DLL" || ext == L"SYS" || ext == L"MSI" || ext == L"CPL") {
        return dark ? Tone{ Rgb(0x0C, 0x30, 0x55), Rgb(0x85, 0xB7, 0xEB) }
                    : Tone{ Rgb(0xE6, 0xF1, 0xFB), Rgb(0x18, 0x5F, 0xA5) };   // 可执行/库 → 蓝
    }
    if (ext == L"TXT" || ext == L"DOC" || ext == L"DOCX" || ext == L"XLS" || ext == L"XLSX" ||
        ext == L"PDF" || ext == L"PPT" || ext == L"PPTX" || ext == L"MD") {
        return dark ? Tone{ Rgb(0x04, 0x40, 0x30), Rgb(0x5D, 0xCA, 0xA5) }
                    : Tone{ Rgb(0xE1, 0xF5, 0xEE), Rgb(0x0F, 0x6E, 0x56) };   // 文档 → 青
    }
    if (ext == L"ZIP" || ext == L"RAR" || ext == L"7Z" || ext == L"ISO") {
        return dark ? Tone{ Rgb(0x4A, 0x30, 0x08), Rgb(0xF0, 0xB5, 0x54) }
                    : Tone{ Rgb(0xFA, 0xEE, 0xDA), Rgb(0x85, 0x4F, 0x0B) };   // 压缩包 → 琥珀
    }
    if (ext == L"JPG" || ext == L"PNG" || ext == L"MP4" || ext == L"MP3" || ext == L"MOV") {
        return dark ? Tone{ Rgb(0x4E, 0x1B, 0x30), Rgb(0xED, 0x93, 0xB1) }
                    : Tone{ Rgb(0xFB, 0xEA, 0xF0), Rgb(0x99, 0x35, 0x56) };   // 媒体 → 粉
    }
    return dark ? Tone{ Rgb(0x3A, 0x3A, 0x38), Rgb(0xB4, 0xB2, 0xA9) }
                : Tone{ Rgb(0xF1, 0xEF, 0xE8), Rgb(0x5F, 0x5E, 0x5A) };
}

// 文件类型图标块：文件夹用字形，其它显示扩展名
Border MakeTile(UiItem const& it, bool dark) {
    Tone tone = TileTone(it.path, it.dir, dark);
    Border b;
    b.Width(24);
    b.Height(24);
    b.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
    b.Background(SolidColorBrush(tone.bg));
    b.VerticalAlignment(VerticalAlignment::Center);
    if (it.dir) {
        FontIcon icon;
        icon.Glyph(hstring(L"\uE8B7"));
        icon.FontSize(13);
        icon.Foreground(SolidColorBrush(tone.fg));
        b.Child(icon);
    } else {
        const size_t dot = it.path.find_last_of(L'.');
        std::wstring ext = dot == std::wstring::npos ? L"?" : it.path.substr(dot + 1);
        if (ext.size() > 3) ext.resize(3);
        for (auto& c : ext) c = static_cast<wchar_t>(towupper(c));
        auto tb = MakeText(hstring(ext), 9.5, SolidColorBrush(tone.fg), false);
        tb.HorizontalAlignment(HorizontalAlignment::Center);
        b.Child(tb);
    }
    return b;
}

// 进程头像：首字母圆标；受保护进程显示 "?" 警示色
Border MakeAvatar(UiOwner const& o, bool dark) {
    Border b;
    b.Width(24);
    b.Height(24);
    b.CornerRadius(CornerRadius{ 99, 99, 99, 99 });
    b.VerticalAlignment(VerticalAlignment::Center);
    Windows::UI::Color bg, fg;
    if (o.prot) {
        bg = dark ? Windows::UI::ColorHelper::FromArgb(255, 0x4A, 0x30, 0x08) : Windows::UI::ColorHelper::FromArgb(255, 0xFA, 0xEE, 0xDA);
        fg = dark ? Windows::UI::ColorHelper::FromArgb(255, 0xFF, 0xBC, 0x74) : Windows::UI::ColorHelper::FromArgb(255, 0x85, 0x4F, 0x0B);
    } else {
        bg = dark ? Windows::UI::ColorHelper::FromArgb(255, 0x0C, 0x30, 0x55) : Windows::UI::ColorHelper::FromArgb(255, 0xE6, 0xF1, 0xFB);
        fg = dark ? Windows::UI::ColorHelper::FromArgb(255, 0x85, 0xB7, 0xEB) : Windows::UI::ColorHelper::FromArgb(255, 0x18, 0x5F, 0xA5);
    }
    b.Background(SolidColorBrush(bg));
    wchar_t ch = L'?';
    if (!o.name.empty()) ch = static_cast<wchar_t>(towupper(o.name[0]));
    auto tb = MakeText(hstring(std::wstring(1, ch)), 11, SolidColorBrush(fg), false);
    tb.HorizontalAlignment(HorizontalAlignment::Center);
    b.Child(tb);
    return b;
}

// 一轮扫描的结果，跨线程搬运
struct ScanOutcome {
    uint64_t id = 0;
    UiItem item;
    bool remove = false;
};

}  // namespace

MainWindow::MainWindow() {
    AppTrace(L"ctor: 开始 InitializeComponent");
    try {
        InitializeComponent();
    } catch (winrt::hresult_error const& e) {
        AppTrace(L"ctor: InitializeComponent 抛出 hr=0x" +
                     std::to_wstring(static_cast<uint32_t>(e.code())) +
                     L" msg=" + std::wstring(e.message().c_str()));
        throw;
    }
    AppTrace(L"ctor: InitializeComponent 完成");

    ul::LogInit(L"Unlocker");
    AppTrace(L"ctor: LogInit 完成");
    ul::PathResolver::Init();
    std::wstring perr;
    if (!ul::PolicyGuard::Init(&perr) || ul::PolicyGuard::FailClosed()) failClosed_ = true;
    ul::HandleScanner::Init(nullptr);
    AppTrace(L"ctor: core 初始化完成");

    queue_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

    SetupWindow();

    // 从命令行 / 右键菜单带来的初始路径
    auto initial = ul::TakeInitialPaths();
    if (!initial.empty()) AddPaths(initial);
}

MainWindow::~MainWindow() {
    alive_.store(false);
}

namespace {

// ------------------------------------------------------------ 窗口位置记忆
// 存在 HKCU\Software\Unlocker 下（不再往 exe 旁边写配置文件，免得出 UAC 虚拟化问题）
constexpr wchar_t kWindowKey[] = L"Software\\Unlocker";
constexpr wchar_t kWindowValue[] = L"WindowPlacement";

// 尺寸常量一律按 **DIP**（XAML 的布局单位）表达，落到 AppWindow 时再乘 DPI 缩放。
// 820x560 是「两张卡片 + 工具栏 + 底部操作区」不重叠的下限；980 DIP 是 XAML 里
// AdaptiveTrigger 切换成纯图标按钮的阈值。
constexpr int kMinWindowWidth = 820;   // DIP
constexpr int kMinWindowHeight = 560;  // DIP
constexpr int kDefaultWindowWidth = 1040;  // DIP
constexpr int kDefaultWindowHeight = 720;  // DIP

// 把矩形夹进所在显示器的工作区（换过显示器 / 改过缩放时，旧坐标可能整块落在屏幕外）
void ClampToWorkArea(RECT& rc, int minW, int minH) {    HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return;

    const RECT& wa = mi.rcWork;
    const int maxW = static_cast<int>(wa.right - wa.left);
    const int maxH = static_cast<int>(wa.bottom - wa.top);
    const int w = std::clamp(static_cast<int>(rc.right - rc.left), minW, maxW);
    const int h = std::clamp(static_cast<int>(rc.bottom - rc.top), minH, maxH);
    const int x = std::clamp<int>(rc.left, wa.left, wa.right - w);
    const int y = std::clamp<int>(rc.top, wa.top, wa.bottom - h);
    rc = { x, y, x + w, y + h };
}

bool LoadWindowRect(RECT& rc, int minW, int minH) {
    DWORD size = sizeof(RECT);
    if (RegGetValueW(HKEY_CURRENT_USER, kWindowKey, kWindowValue, RRF_RT_REG_BINARY, nullptr, &rc,
                     &size) != ERROR_SUCCESS) {
        return false;
    }
    if (size != sizeof(RECT)) return false;
    if (rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0) return false;
    // 上次用的显示器可能已经拔了 / 改过缩放：夹回工作区并抬起最小尺寸
    ClampToWorkArea(rc, minW, minH);
    return true;
}

void SaveWindowRect(RECT const& rc) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kWindowKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, kWindowValue, 0, REG_BINARY, reinterpret_cast<BYTE const*>(&rc), sizeof(RECT));
    RegCloseKey(key);
}

}  // namespace

void MainWindow::ApplyNarrowLayout() {
    // 980 DIP 是「六个按钮带文案刚好排得下」的临界值；再窄就只留图标 + 悬停提示
    const bool wide = RootGrid().ActualWidth() >= 980.0;
    const auto vis = wide ? Visibility::Visible : Visibility::Collapsed;
    UnlockText().Visibility(vis);
    DeleteText().Visibility(vis);
    RenameText().Visibility(vis);
    MoveText().Visibility(vis);
    CopyText().Visibility(vis);
    RescanText().Visibility(vis);
}

void MainWindow::SetupWindow() {
    Title(L"Unlocker");

    // 系统亚克力：WinUI 3 自带 DesktopAcrylicBackdrop，不需要自己调 DWM 属性
    SystemBackdrop(Media::DesktopAcrylicBackdrop());

    // 内容延伸进标题栏：图标 + 标题 + 副标题自绘，Snap Layout / 双击最大化保留
    AppTrace(L"setup: ExtendsContentIntoTitleBar 前");
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(TitleBarGrid());
    AppTrace(L"setup: SetTitleBar 完成");

    // 最小尺寸限制：820x560 是按**DIP**算的布局下限（XAML 窄布局从 980 DIP 起切换成纯图标按钮）。
    // AppWindow 的尺寸单位是物理像素，150% 缩放下必须乘 scale，否则实际可用 DIP 只有三分之二，
    // 卡片会被压扁、底部操作区被裁掉 —— 这正是「缩小窗口后内容被遮挡」的根因之一。
    const double scale = static_cast<double>(GetDpiForWindow(Hwnd())) / 96.0;
    const auto px = [scale](int dip) {
        return static_cast<int32_t>(dip * scale + 0.5);
    };
    const int minWpx = px(kMinWindowWidth);
    const int minHpx = px(kMinWindowHeight);

    if (auto presenter =
            AppWindow().Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>()) {
        presenter.PreferredMinimumWidth(
            winrt::Windows::Foundation::PropertyValue::CreateInt32(minWpx)
                .as<winrt::Windows::Foundation::IReference<int32_t>>());
        presenter.PreferredMinimumHeight(
            winrt::Windows::Foundation::PropertyValue::CreateInt32(minHpx)
                .as<winrt::Windows::Foundation::IReference<int32_t>>());
    }

    // 初始尺寸：优先恢复上次的位置（夹进当前工作区 + 不低于最小尺寸），其次按 DIP 设计尺寸居中
    RECT saved{};
    if (LoadWindowRect(saved, minWpx, minHpx)) {
        AppWindow().MoveAndResize(
            { saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top });
    } else {
        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        const RECT wa = (mon && GetMonitorInfoW(mon, &mi))
                            ? mi.rcWork
                            : RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        const int workW = static_cast<int>(wa.right - wa.left);
        const int workH = static_cast<int>(wa.bottom - wa.top);
        const int w = std::min(px(kDefaultWindowWidth), workW);
        const int h = std::min(px(kDefaultWindowHeight), workH);
        const int x = wa.left + (workW - w) / 2;
        const int y = wa.top + (workH - h) / 2;
        AppWindow().MoveAndResize({ x, y, w, h });
    }
    AppTrace(L"setup: 初始尺寸完成");
    {
        // 记一行尺寸，方便核对 DPI 缩放 / 最小尺寸有没有按预期生效（W1 排障用）
        const auto pos = AppWindow().Position();
        const auto size = AppWindow().Size();
        ul::LogI(L"窗口 " + std::to_wstring(size.Width) + L"x" + std::to_wstring(size.Height) +
                 L" @" + std::to_wstring(pos.X) + L"," + std::to_wstring(pos.Y) + L"（DPI 缩放 " +
                 std::to_wstring(scale) + L"）");
    }

    // 宽度自适应：<980 DIP 时底部六个操作按钮退化为纯图标（否则会横向挤爆、把「重新扫描」裁掉）
    RootGrid().SizeChanged([this](auto&&, auto&&) {
        if (alive_.load()) ApplyNarrowLayout();
    });
    ApplyNarrowLayout();

    // 关闭时记住位置（最大化/最小化状态不覆盖，只记 Restored 的窗口框）
    Closed([this](auto&&, auto&&) {
        auto appWindow = AppWindow();
        if (!appWindow) return;
        if (auto presenter = appWindow.Presenter()
                                 .try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>()) {
            if (presenter.State() !=
                winrt::Microsoft::UI::Windowing::OverlappedPresenterState::Restored) {
                return;
            }
        }
        const auto pos = appWindow.Position();
        const auto size = appWindow.Size();
        SaveWindowRect({ pos.X, pos.Y, pos.X + size.Width, pos.Y + size.Height });
    });

    // ------------------------------------------------------------ 事件绑定
    // 注意：MainWindow.xaml 里没有写 Click=，绑定集中在这里。
    // （M5.1 期间这一整块是缺的 —— 界面能显示，但所有按钮都没反应。）
    AddFileButton().Click({ this, &MainWindow::OnAddFiles });
    AddFolderButton().Click({ this, &MainWindow::OnAddFolders });
    DeepScanButton().Click({ this, &MainWindow::OnDeepScan });
    UnlockButton().Click({ this, &MainWindow::OnUnlock });
    DeleteButton().Click({ this, &MainWindow::OnDelete });
    MoveButton().Click({ this, &MainWindow::OnMove });
    CopyButton().Click({ this, &MainWindow::OnCopy });
    RescanButton().Click({ this, &MainWindow::OnRescan });
    // OnRename 是协程（fire_and_forget），不能直接当委托用，套一层 lambda 启动
    RenameButton().Click([this](auto&&, auto&&) {
        if (alive_.load()) OnRename(nullptr, nullptr);
    });

    // 按钮悬停提示
    ToolTipService::SetToolTip(AddFileButton(), box_value(Hs(L"选择一个或多个文件加入列表")));
    ToolTipService::SetToolTip(AddFolderButton(), box_value(Hs(L"选择一个文件夹加入列表")));
    ToolTipService::SetToolTip(DeepScanButton(), box_value(Hs(L"以管理员权限重新扫描，可以看到服务与受保护进程的占用")));
    ToolTipService::SetToolTip(ForceCheck(), box_value(Hs(L"允许终止占用进程后再操作（系统关键进程仍会被拒绝）")));
    ToolTipService::SetToolTip(RescanButton(), box_value(Hs(L"重新扫描列表中所有项目的占用状态")));

    elevated_ = ul::IsElevated();

    if (failClosed_) {
        Banner().Severity(InfoBarSeverity::Error);
        Banner().Title(L"规则表自检失败");
        Banner().Message(L"已进入保护模式：所有操作都会被拒绝。请重新安装本程序。");
        Banner().IsOpen(true);
    } else if (elevated_) {
        Banner().Severity(InfoBarSeverity::Warning);
        Banner().Title(L"当前以管理员权限运行");
        Banner().Message(L"推荐以普通权限启动，拖放与右键菜单会更稳定。");
        Banner().IsOpen(true);
        ul::PatchDropForElevatedUi(Hwnd());
    }

    SetStatus(elevated_ ? L"执行组件：运行中（管理员权限 UI）"
                        : L"执行组件：未启动（首次写操作时会请求管理员权限）");
    SetBusy(false);
    RefreshEmptyHint();
    UpdateSummary();

    // 深浅色切换时行内的自定义配色需要重建
    RootGrid().ActualThemeChanged([this](auto&&, auto&&) {
        if (alive_.load()) RebuildFileList();
    });

    // 右键菜单 / 第二个实例通过通道 A 把路径送进来
    // 注意：不用 get_weak/get_self —— 本项目的投影没有实现侧 producer 特化，
    // get_self 会触发 producer<> 实例化失败；用 this + alive_ 更直接。
    ul::StartUiPipeListenerFn([this](std::vector<std::wstring> paths) {
        if (!alive_.load()) return;
        queue_.TryEnqueue([this, paths = std::move(paths)] {
            if (alive_.load()) AddPaths(paths);
        });
    });
}

HWND MainWindow::Hwnd() {
    return winrt::Microsoft::UI::GetWindowFromWindowId(AppWindow().Id());
}

void MainWindow::SetStatus(std::wstring const& text) {
    StatusText().Text(Hs(text));
}

void MainWindow::ShowMessage(std::wstring const& title, std::wstring const& text) {
    ContentDialog dlg;
    dlg.XamlRoot(RootGrid().XamlRoot());
    dlg.Title(box_value(Hs(title)));
    dlg.Content(box_value(Hs(text)));
    dlg.CloseButtonText(L"确定");
    // fire-and-forget：调用方在 UI 线程，这里不等待结果
    dlg.ShowAsync();
}

void MainWindow::SetBusy(bool busy) {
    busy_ = busy;
    BusyRing().IsActive(busy);
    ScanProgress().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
    for (auto const& b : { AddFileButton(), AddFolderButton(), DeepScanButton(), UnlockButton(),
                           DeleteButton(), RenameButton(), MoveButton(), CopyButton(), RescanButton() }) {
        b.IsEnabled(!busy);
    }
    ForceCheck().IsEnabled(!busy);
}

void MainWindow::RefreshEmptyHint() {
    EmptyHint().Visibility(items_.empty() ? Visibility::Visible : Visibility::Collapsed);
}

void MainWindow::UpdateSummary() {
    int locked = 0, ok = 0, confirm = 0, deny = 0, pending = 0;
    for (auto const& it : items_) {
        if (it.verdict == ul::Verdict::Deny) { ++deny; continue; }
        if (!it.scanned) { ++pending; continue; }
        if (it.verdict == ul::Verdict::Confirm) ++confirm;
        switch (it.state) {
            case ul::OccupancyState::Locked:
            case ul::OccupancyState::ImageMapped:
            case ul::OccupancyState::LockedByKernel:
            case ul::OccupancyState::AccessDenied:
                ++locked; break;
            case ul::OccupancyState::Free:
            case ul::OccupancyState::Opened:
                ++ok; break;
            default: break;
        }
    }
    std::wstring s;
    if (deny)     s += std::to_wstring(deny) + L" 个已阻止";
    if (locked)  { if (!s.empty()) s += L" · "; s += std::to_wstring(locked) + L" 个锁定"; }
    if (confirm) { if (!s.empty()) s += L" · "; s += std::to_wstring(confirm) + L" 个需确认"; }
    if (ok)      { if (!s.empty()) s += L" · "; s += std::to_wstring(ok) + L" 个可操作"; }
    if (pending) { if (!s.empty()) s += L" · "; s += std::to_wstring(pending) + L" 个待扫描"; }
    SummaryText().Text(Hs(s));
}

void MainWindow::RebuildFileList() {
    FileList().Items().Clear();
    rows_.clear();

    const bool dark = IsDark(RootGrid().ActualTheme());

    for (auto const& it : items_) {
        Grid row;
        row.Padding(Thickness{ 12, 4, 12, 4 });
        row.ColumnSpacing(10);

        // 黑名单 Deny 行：红色左描边，一眼可辨
        if (it.verdict == ul::Verdict::Deny) {
            row.BorderBrush(SolidColorBrush(dark ? Rgb(0xE2, 0x4B, 0x4B) : Rgb(0xA3, 0x2D, 0x2D)));
            row.BorderThickness(Thickness{ 3, 0, 0, 0 });
        }

        row.ColumnDefinitions().Append(FixedCol(24));   // 类型图标
        row.ColumnDefinitions().Append(StarCol());      // 路径
        row.ColumnDefinitions().Append(FixedCol(64));   // 大小
        row.ColumnDefinitions().Append(FixedCol(170));  // 占用者
        row.ColumnDefinitions().Append(FixedCol(96));   // 状态徽章

        auto tile = MakeTile(it, dark);
        Grid::SetColumn(tile, 0);
        row.Children().Append(tile);

        // 主行：路径；黑名单行附上规则 ID
        std::wstring pathText = it.path;
        if (it.verdict == ul::Verdict::Deny && !it.rule.empty()) pathText += L"   " + it.rule;
        auto path = MakeText(Hs(pathText), 13, TextPrimary(dark), true);
        Grid::SetColumn(path, 1);
        row.Children().Append(path);

        auto size = MakeText(Hs(it.dir ? L"<目录>" : ul::FormatSize(it.size)), 12.5, TextSecondary(dark), false);
        Grid::SetColumn(size, 2);
        row.Children().Append(size);

        auto owners = MakeText(Hs(OwnersSummary(it)), 12.5, TextSecondary(dark), true);
        Grid::SetColumn(owners, 3);
        row.Children().Append(owners);

        auto badge = MakeBadge(Hs(StateText(it)), ToneFor(it, dark));
        Grid::SetColumn(badge, 4);
        row.Children().Append(badge);

        // Deny / Confirm 行把黑名单原因放进悬停提示
        if ((it.verdict != ul::Verdict::Allow) && !it.reason.empty()) {
            ToolTipService::SetToolTip(row, box_value(Hs(it.rule + L"：" + it.reason)));
        }

        FileList().Items().Append(row);
        rows_.push_back(row);
    }

    // 有项目正在扫描时显示底部进度条
    bool anyScanning = false;
    for (auto const& it : items_) if (it.scanning && !it.scanned) anyScanning = true;
    ScanProgress().Visibility(anyScanning ? Visibility::Visible : Visibility::Collapsed);

    RefreshEmptyHint();
    UpdateSummary();
    RebuildProcessList();
}

void MainWindow::RebuildProcessList() {
    ProcessList().Items().Clear();

    std::vector<UiItem const*> targets;
    auto sel = SelectedIndices();
    if (sel.empty()) {
        for (auto const& it : items_) targets.push_back(&it);
    } else {
        for (size_t i : sel) {
            if (i < items_.size()) targets.push_back(&items_[i]);
        }
    }

    std::vector<UiOwner> merged;
    for (auto const* it : targets) {
        for (auto const& o : it->owners) {
            bool found = false;
            for (auto& m : merged) {
                if (m.pid == o.pid) { m.handles += o.handles; found = true; break; }
            }
            if (!found) merged.push_back(o);
        }
    }

    const bool dark = IsDark(RootGrid().ActualTheme());
    for (auto const& o : merged) {
        Grid row;
        row.Padding(Thickness{ 12, 4, 12, 4 });
        row.ColumnSpacing(10);
        row.ColumnDefinitions().Append(FixedCol(24));   // 头像
        row.ColumnDefinitions().Append(FixedCol(200));  // 进程名
        row.ColumnDefinitions().Append(FixedCol(64));   // PID
        row.ColumnDefinitions().Append(StarCol());      // 描述
        row.ColumnDefinitions().Append(FixedCol(60));   // 句柄
        row.ColumnDefinitions().Append(FixedCol(64));   // 状态

        auto avatar = MakeAvatar(o, dark);
        Grid::SetColumn(avatar, 0);
        row.Children().Append(avatar);

        auto name = MakeText(Hs(o.name.empty() ? (L"PID " + std::to_wstring(o.pid)) : o.name), 13, TextPrimary(dark), true);
        Grid::SetColumn(name, 1);
        row.Children().Append(name);

        auto pid = MakeText(Hs(std::to_wstring(o.pid)), 12.5, TextSecondary(dark), false);
        Grid::SetColumn(pid, 2);
        row.Children().Append(pid);

        auto desc = MakeText(Hs(o.desc.empty() ? L"—" : o.desc), 12.5, TextSecondary(dark), true);
        Grid::SetColumn(desc, 3);
        row.Children().Append(desc);

        // 句柄数胶囊徽章
        Border hb;
        hb.CornerRadius(CornerRadius{ 99, 99, 99, 99 });
        hb.Padding(Thickness{ 7, 1, 7, 2 });
        hb.Background(SolidColorBrush(dark ? Rgb(0x3A, 0x3A, 0x38) : Rgb(0xF1, 0xEF, 0xE8)));
        hb.VerticalAlignment(VerticalAlignment::Center);
        hb.HorizontalAlignment(HorizontalAlignment::Left);
        auto htb = MakeText(Hs(L"×" + std::to_wstring(o.handles)), 11,
                            SolidColorBrush(dark ? Rgb(0xC8, 0xC8, 0xC8) : Rgb(0x5F, 0x5E, 0x5A)), false);
        hb.Child(htb);
        Grid::SetColumn(hb, 4);
        row.Children().Append(hb);

        // 进程状态：受保护 > 映像 > 占用
        std::wstring st = L"占用";
        Windows::UI::Color stc = dark ? Rgb(0xFF, 0x8A, 0x8A) : Rgb(0xC0, 0x00, 0x00);
        if (o.prot) { st = L"受保护"; stc = dark ? Rgb(0xFF, 0xBC, 0x74) : Rgb(0x85, 0x4F, 0x0B); }
        else if (o.image) { st = L"映像"; stc = dark ? Rgb(0xFF, 0xB8, 0x6B) : Rgb(0xC8, 0x60, 0x00); }
        auto status = MakeText(Hs(st), 12, SolidColorBrush(stc), false);
        Grid::SetColumn(status, 5);
        row.Children().Append(status);

        ProcessList().Items().Append(row);
    }

    // 结论可能被「看不到的进程」影响时，末尾汇总一行
    int skipped = 0;
    for (auto const* it : targets) {
        if (it->state == ul::OccupancyState::Locked || it->state == ul::OccupancyState::LockedByKernel ||
            it->state == ul::OccupancyState::AccessDenied) {
            skipped += static_cast<int>(it->skipped.size());
        }
    }
    if (skipped > 0) {
        Grid row;
        row.Padding(Thickness{ 12, 6, 12, 6 });
        auto text = MakeText(Hs(L"另有 " + std::to_wstring(skipped) +
                                L" 个受保护进程无法检查（可能是系统进程或安全软件）"),
                             12, TextSecondary(dark), true);
        row.Children().Append(text);
        ProcessList().Items().Append(row);
    }
}

std::vector<size_t> MainWindow::SelectedIndices() {
    std::vector<size_t> out;
    auto selected = FileList().SelectedItems();
    for (auto const& obj : selected) {
        auto row = obj.try_as<Grid>();
        if (!row) continue;
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i] == row) { out.push_back(i); break; }
        }
    }
    return out;
}

void MainWindow::AddPaths(std::vector<std::wstring> const& raw) {
    if (raw.empty()) return;
    if (raw.size() > 500) {
        SetStatus(L"一次拖入了 " + std::to_wstring(raw.size()) + L" 个对象，超过单批 500 的上限，请分批处理");
        return;
    }

    std::vector<std::pair<uint64_t, std::wstring>> toScan;
    int rejected = 0;

    for (auto const& rawPath : raw) {
        std::wstring path = ul::Trim(rawPath);
        if (path.empty()) continue;

        if (ul::IsUncPath(path) && !ul::IStartsWith(path, L"\\\\?\\")) { ++rejected; continue; }
        if (path.find(L"\\$Recycle.Bin\\") != std::wstring::npos) { ++rejected; continue; }

        // .lnk 解析出的目标仍然要过黑名单（防「建个快捷方式指向 ntoskrnl」）
        if (ul::IEndsWith(path, L".lnk")) {
            IShellLinkW* link = nullptr;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || !link) {
                ++rejected;
                continue;
            }
            IPersistFile* file = nullptr;
            std::wstring target;
            if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file) {
                if (SUCCEEDED(file->Load(path.c_str(), STGM_READ)) && SUCCEEDED(link->Resolve(nullptr, SLR_NO_UI))) {
                    wchar_t buf[MAX_PATH * 4]{};
                    if (SUCCEEDED(link->GetPath(buf, ARRAYSIZE(buf), nullptr, 0)) && buf[0]) target = buf;
                }
                file->Release();
            }
            link->Release();
            if (target.empty()) { ++rejected; continue; }
            path = target;
        }

        bool resolved = false;
        std::wstring canonical = ul::PathResolver::CanonicalPath(path, &resolved);
        if (canonical.empty()) canonical = ul::PathResolver::Normalize(path);
        if (canonical.empty()) continue;

        bool dup = false;
        for (auto const& it : items_) {
            if (ul::IEquals(it.canonical, canonical)) { dup = true; break; }
        }
        if (dup) continue;

        ul::PolicyResult pol = ul::PolicyGuard::Evaluate(path);
        UiItem it;
        it.id = nextId_++;
        it.path = path;
        it.canonical = canonical;
        it.verdict = pol.verdict;
        it.rule = ul::PolicyGuard::RuleName(pol.ruleId);
        it.reason = pol.reason;
        it.scanning = true;
        items_.push_back(std::move(it));
        toScan.emplace_back(items_.back().id, items_.back().path);
    }

    if (toScan.empty()) {
        SetStatus(rejected ? L"没有可加入的路径（UNC / 回收站 / 无法解析的快捷方式已拒绝）" : L"没有新的可加入项");
        return;
    }

    RebuildFileList();
    // 留一条可核对的痕迹：拖放 / 单实例转发到底有没有送到（W2 / W9 的验收依据）
    ul::LogI(L"UI 加入 " + std::to_wstring(toScan.size()) + L" 项（拒绝 " +
             std::to_wstring(rejected) + L" 项），开始本地扫描");
    SetStatus(L"正在扫描占用进程…");
    StartLocalScan(toScan);
}

void MainWindow::StartLocalScan(std::vector<std::pair<uint64_t, std::wstring>> targets) {
    if (targets.empty()) return;
    const bool elevated = elevated_;

    std::thread([this, targets, elevated] {
        std::vector<ScanOutcome> outcomes;
        for (auto const& [id, path] : targets) {
            ul::FileReport rep = ul::HandleScanner::Scan(path, ul::ScanOptions{ elevated, true });

            ScanOutcome o;
            o.id = id;
            o.remove = (rep.state == ul::OccupancyState::NotFound);
            UiItem& it = o.item;
            it.id = id;
            it.path = path;
            it.canonical = rep.canonical;
            it.state = rep.state;
            it.error = rep.error;
            it.size = rep.size;
            it.dir = rep.directory;
            it.scanned = true;
            it.needElevation = rep.needElevation;
            it.handles = rep.totalFileHandles;
            it.skipped = rep.skippedPids;
            for (auto const& ow : rep.owners) {
                bool dup = false;
                for (auto const& uo : it.owners) if (uo.pid == ow.proc.pid) { dup = true; break; }
                if (dup) continue;
                UiOwner uo;
                uo.pid = ow.proc.pid;
                uo.name = ow.proc.name;
                uo.desc = ow.proc.description;
                uo.fullPath = ow.proc.fullPath;
                uo.handles = ow.proc.handleCount;
                uo.prot = ow.proc.protectedProc;
                uo.image = ow.isImageMapping;
                it.owners.push_back(std::move(uo));
            }
            outcomes.push_back(std::move(o));
        }

        if (!alive_.load()) return;
        {
            MainWindow* impl = this;
            queue_.TryEnqueue([impl, outcomes = std::move(outcomes)]() mutable {
                if (!impl->alive_.load()) return;
                for (auto const& o : outcomes) {
                    for (size_t i = 0; i < impl->items_.size(); ++i) {
                        if (impl->items_[i].id != o.id) continue;
                        if (o.remove) {
                            impl->items_.erase(impl->items_.begin() + i);
                        } else {
                            UiItem merged = o.item;
                            merged.path = impl->items_[i].path;
                            merged.verdict = impl->items_[i].verdict;
                            merged.rule = impl->items_[i].rule;
                            merged.reason = impl->items_[i].reason;
                            impl->items_[i] = std::move(merged);
                        }
                        break;
                    }
                }
                impl->RebuildFileList();
                impl->SetStatus(L"扫描完成");
            });
        }
    }).detach();
}

void MainWindow::StartDeepScan(std::vector<std::pair<uint64_t, std::wstring>> targets) {
    if (targets.empty()) return;
    SetBusy(true);
    SetStatus(L"正在请求管理员权限以深扫…");

    std::thread([this, targets] {
        std::lock_guard<std::mutex> lk(workerMutex_);
        std::wstring err;
        if (!worker_.EnsureStarted(&err)) {
            if (alive_.load()) {
                MainWindow* impl = this;
                queue_.TryEnqueue([impl, err] {
                    if (!impl->alive_.load()) return;
                    impl->SetBusy(false);
                    impl->SetStatus(err);
                });
            }
            return;
        }

        ul::IpcRequest req;
        req.op = ul::IpcOp::Scan;
        req.caller = "GUI";
        for (auto const& kv : targets) req.items.push_back(kv.second);

        ul::json::Value result;
        bool got = false;
        const bool ok = worker_.Request(req, [&](ul::json::Value const& v) {
            if (v.GetString("evt") == "progress") {
                if (alive_.load()) {
                    MainWindow* impl = this;
                    const int cur = static_cast<int>(v.GetNumber("cur", 0));
                    const int total = static_cast<int>(v.GetNumber("total", 0));
                    impl->queue_.TryEnqueue([impl, cur, total] {
                        impl->SetStatus(L"深扫中 " + std::to_wstring(cur) + L"/" + std::to_wstring(total));
                    });
                }
                return;
            }
            result = v;
            got = true;
        }, &err, 300000);

        if (!ok || !got) {
            if (alive_.load()) {
                MainWindow* impl = this;
                queue_.TryEnqueue([impl, err] {
                    if (!impl->alive_.load()) return;
                    impl->SetBusy(false);
                    impl->SetStatus(err.empty() ? L"深扫失败" : err);
                });
            }
            return;
        }

        std::vector<ScanOutcome> outcomes;
        const ul::json::Value* results = result.Find("results");
        if (results && results->type == ul::json::Value::Type::Array) {
            for (size_t i = 0; i < results->arr.size() && i < targets.size(); ++i) {
                ul::json::Value const& o = results->arr[i];
                ScanOutcome out;
                out.id = targets[i].first;
                UiItem& it = out.item;
                it.id = out.id;
                it.path = ul::Utf8ToUtf16(o.GetString("path"));
                it.canonical = ul::Utf8ToUtf16(o.GetString("canonical"));
                it.size = static_cast<unsigned long long>(o.GetNumber("size", 0));
                it.dir = o.GetBool("dir");
                it.error = static_cast<uint32_t>(o.GetNumber("error", 0));
                it.needElevation = o.GetBool("needElevation");
                it.handles = static_cast<int>(o.GetNumber("handles", 0));
                it.verdict = static_cast<ul::Verdict>(o.GetNumber("verdict", 0));
                it.rule = ul::Utf8ToUtf16(o.GetString("rule"));
                it.reason = ul::Utf8ToUtf16(o.GetString("reason"));
                it.scanned = true;

                const std::string st = o.GetString("state");
                if (st == "free") it.state = ul::OccupancyState::Free;
                else if (st == "opened") it.state = ul::OccupancyState::Opened;
                else if (st == "locked") it.state = ul::OccupancyState::Locked;
                else if (st == "imagemapped") it.state = ul::OccupancyState::ImageMapped;
                else if (st == "accessdenied") it.state = ul::OccupancyState::AccessDenied;
                else if (st == "lockedbykernel") it.state = ul::OccupancyState::LockedByKernel;
                else if (st == "notfound") it.state = ul::OccupancyState::NotFound;
                else it.state = ul::OccupancyState::Error;

                const ul::json::Value* owners = o.Find("owners");
                if (owners && owners->type == ul::json::Value::Type::Array) {
                    for (auto const& ow : owners->arr) {
                        UiOwner uo;
                        uo.pid = static_cast<uint32_t>(ow.GetNumber("pid", 0));
                        uo.name = ul::Utf8ToUtf16(ow.GetString("name"));
                        uo.desc = ul::Utf8ToUtf16(ow.GetString("desc"));
                        uo.fullPath = ul::Utf8ToUtf16(ow.GetString("fullPath"));
                        uo.handles = static_cast<int>(ow.GetNumber("handles", 1));
                        uo.prot = ow.GetBool("protected");
                        uo.image = ow.GetBool("image");
                        it.owners.push_back(std::move(uo));
                    }
                }
                outcomes.push_back(std::move(out));
            }
        }

        if (!alive_.load()) return;
        {
            MainWindow* impl = this;
            queue_.TryEnqueue([impl, outcomes = std::move(outcomes)]() mutable {
                if (!impl->alive_.load()) return;
                for (auto const& o : outcomes) {
                    for (auto& it : impl->items_) {
                        if (it.id != o.id) continue;
                        UiItem merged = o.item;
                        merged.path = it.path;
                        merged.verdict = it.verdict;
                        merged.rule = it.rule;
                        merged.reason = it.reason;
                        it = std::move(merged);
                        break;
                    }
                }
                impl->SetBusy(false);
                impl->RebuildFileList();
                impl->SetStatus(L"深扫完成（管理员）");
            });
        }
    }).detach();
}

void MainWindow::StartOp(ul::IpcOp op, std::wstring arg) {
    if (busy_) return;

    auto sel = SelectedIndices();
    std::vector<std::wstring> paths;
    int denied = 0;
    auto add = [&](UiItem const& it) {
        if (it.verdict == ul::Verdict::Deny) { ++denied; return; }
        if (it.state == ul::OccupancyState::NotFound) return;
        paths.push_back(it.path);
    };
    if (sel.empty()) {
        for (auto const& it : items_) add(it);
    } else {
        for (size_t i : sel) if (i < items_.size()) add(items_[i]);
    }
    if (paths.empty()) {
        SetStatus(denied ? L"选中的项目全部被内置黑名单阻止" : L"没有可操作的项目，请先添加文件");
        return;
    }

    SetBusy(true);
    SetStatus(L"正在请求管理员权限…（首次操作会弹出 UAC）");
    const bool force = ForceCheck().IsChecked().GetBoolean();

    std::thread([this, op, arg, paths, force] {
        std::lock_guard<std::mutex> lk(workerMutex_);
        std::wstring err;
        if (!worker_.EnsureStarted(&err)) {
            if (alive_.load()) {
                MainWindow* impl = this;
                queue_.TryEnqueue([impl, err] {
                    if (!impl->alive_.load()) return;
                    impl->SetBusy(false);
                    impl->SetStatus(err);
                });
            }
            return;
        }

        ul::IpcRequest req;
        req.op = op;
        req.items = paths;
        req.arg = arg;
        req.force = force;
        req.caller = "GUI";

        ul::json::Value result;
        bool got = false;
        const bool ok = worker_.Request(req, [&](ul::json::Value const& v) {
            if (v.GetString("evt") == "progress") {
                if (alive_.load()) {
                    MainWindow* impl = this;
                    const int cur = static_cast<int>(v.GetNumber("cur", 0));
                    const int total = static_cast<int>(v.GetNumber("total", 0));
                    impl->queue_.TryEnqueue([impl, cur, total] {
                        impl->SetStatus(L"处理中 " + std::to_wstring(cur) + L"/" + std::to_wstring(total));
                    });
                }
                return;
            }
            result = v;
            got = true;
        }, &err, 300000);

        std::wstring summary;
        std::wstring detail;
        if (!ok || !got) {
            summary = err;
        } else {
            summary = L"成功 " + std::to_wstring(static_cast<int>(result.GetNumber("okCount", 0))) +
                      L"，失败 " + std::to_wstring(static_cast<int>(result.GetNumber("failCount", 0))) +
                      L"，跳过 " + std::to_wstring(static_cast<int>(result.GetNumber("skipCount", 0)));
            if (result.GetBool("reboot", false)) summary += L"\n有项目已排队到重启后执行。";

            const ul::json::Value* results = result.Find("results");
            if (results && results->type == ul::json::Value::Type::Array) {
                for (auto const& it : results->arr) {
                    if (it.GetBool("ok") && !it.GetBool("skipped")) continue;
                    if (detail.size() > 3000) break;
                    detail += (it.GetBool("skipped") ? L"[跳过] " : L"[失败] ") +
                              ul::Utf8ToUtf16(it.GetString("path")) + L"\n";
                    const std::wstring rule = ul::Utf8ToUtf16(it.GetString("rule"));
                    if (!rule.empty()) detail += L"    规则 " + rule + L"\n";
                    const std::wstring msg = ul::Utf8ToUtf16(it.GetString("msg"));
                    if (!msg.empty()) detail += L"    " + msg + L"\n";
                }
            }
        }

        if (alive_.load()) {
            MainWindow* impl = this;
            queue_.TryEnqueue([impl, summary, detail] {
                if (!impl->alive_.load()) return;
                impl->SetBusy(false);
                impl->SetStatus(summary);
                std::wstring text = summary;
                if (!detail.empty()) text += L"\n\n" + detail;
                impl->ShowMessage(L"Unlocker", text);
                // 操作完成后重新扫描，状态列立刻反映真实结果
                std::vector<std::pair<uint64_t, std::wstring>> rescan;
                for (auto& it : impl->items_) {
                    if (it.verdict == ul::Verdict::Deny) continue;
                    it.scanned = false;
                    it.scanning = true;
                    it.owners.clear();
                    rescan.emplace_back(it.id, it.path);
                }
                impl->RebuildFileList();
                if (!rescan.empty()) impl->StartLocalScan(rescan);
            });
        }
    }).detach();
}

// ---------------------------------------------------------------- 事件
void MainWindow::OnDragOver(IInspectable const&, DragEventArgs const& e) {
    if (e.DataView().Contains(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems())) {
        e.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Link);
        e.DragUIOverride().Caption(L"添加到 Unlocker");
        e.DragUIOverride().IsCaptionVisible(true);
    }
}

fire_and_forget MainWindow::OnDrop(IInspectable const&, DragEventArgs const& e) {
    if (!e.DataView().Contains(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems())) co_return;
    auto items = co_await e.DataView().GetStorageItemsAsync();
    std::vector<std::wstring> paths;
    for (auto const& item : items) {
        auto path = item.Path();
        if (!path.empty()) paths.emplace_back(path.c_str());
    }
    AddPaths(paths);
}

void MainWindow::OnAddFiles(IInspectable const&, RoutedEventArgs const&) {
    auto files = ul::PickFiles(Hwnd());
    if (!files.empty()) AddPaths(files);
}

void MainWindow::OnAddFolders(IInspectable const&, RoutedEventArgs const&) {
    auto dirs = ul::PickFolders(Hwnd());
    if (!dirs.empty()) AddPaths(dirs);
}

void MainWindow::OnDeepScan(IInspectable const&, RoutedEventArgs const&) {
    std::vector<std::pair<uint64_t, std::wstring>> targets;
    for (auto const& it : items_) {
        if (it.verdict == ul::Verdict::Deny) continue;
        targets.emplace_back(it.id, it.path);
    }
    if (targets.empty()) {
        SetStatus(L"没有可扫描的项目");
        return;
    }
    StartDeepScan(targets);
}

void MainWindow::OnUnlock(IInspectable const&, RoutedEventArgs const&) { StartOp(ul::IpcOp::Unlock, L""); }
void MainWindow::OnDelete(IInspectable const&, RoutedEventArgs const&) { StartOp(ul::IpcOp::Delete, L""); }
void MainWindow::OnMove(IInspectable const&, RoutedEventArgs const&) {
    auto dirs = ul::PickFolders(Hwnd());
    if (!dirs.empty()) StartOp(ul::IpcOp::Move, dirs.front());
}
void MainWindow::OnCopy(IInspectable const&, RoutedEventArgs const&) {
    auto dirs = ul::PickFolders(Hwnd());
    if (!dirs.empty()) StartOp(ul::IpcOp::Copy, dirs.front());
}

fire_and_forget MainWindow::OnRename(IInspectable const&, RoutedEventArgs const&) {
    auto sel = SelectedIndices();
    if (sel.empty()) {
        ShowMessage(L"Unlocker", L"请先选中一个文件。");
        co_return;
    }
    auto const& target = items_[sel.front()];

    TextBox box;
    box.Text(Hs(FileName(target.path)));
    box.SelectAll();
    ContentDialog dlg;
    dlg.XamlRoot(RootGrid().XamlRoot());
    dlg.Title(box_value(L"解锁并改名"));
    dlg.Content(box);
    dlg.PrimaryButtonText(L"改名");
    dlg.CloseButtonText(L"取消");
    dlg.DefaultButton(ContentDialogButton::Primary);

    auto result = co_await dlg.ShowAsync();
    if (result != ContentDialogResult::Primary) co_return;

    std::wstring name = Ws(box.Text());
    if (name.empty() || name.find(L'\\') != std::wstring::npos) {
        ShowMessage(L"Unlocker", L"新名称不能为空，也不能包含路径分隔符。");
        co_return;
    }
    StartOp(ul::IpcOp::Rename, name);
}

void MainWindow::OnRescan(IInspectable const&, RoutedEventArgs const&) {
    std::vector<std::pair<uint64_t, std::wstring>> targets;
    for (auto& it : items_) {
        if (it.verdict == ul::Verdict::Deny) continue;
        it.scanned = false;
        it.scanning = true;
        it.owners.clear();
        targets.emplace_back(it.id, it.path);
    }
    RebuildFileList();
    if (targets.empty()) return;
    SetStatus(L"重新扫描中…");
    StartLocalScan(targets);
}

void MainWindow::OnFileSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    RebuildProcessList();
}

// ---------------------------------------------------------------- 键盘加速键
void MainWindow::OnSelectAll(KeyboardAccelerator const& Key,
                             KeyboardAcceleratorInvokedEventArgs const& args) {
    UNREFERENCED_PARAMETER(Key);
    args.Handled(true);
    if (!alive_.load() || items_.empty()) return;
    FileList().SelectAll();
}

void MainWindow::OnUnlockAccelerator(KeyboardAccelerator const& Key,
                                     KeyboardAcceleratorInvokedEventArgs const& args) {
    UNREFERENCED_PARAMETER(Key);
    args.Handled(true);
    if (alive_.load()) OnUnlock(nullptr, nullptr);
}

void MainWindow::OnDeleteAccelerator(KeyboardAccelerator const& Key,
                                     KeyboardAcceleratorInvokedEventArgs const& args) {
    UNREFERENCED_PARAMETER(Key);
    args.Handled(true);
    if (alive_.load()) OnDelete(nullptr, nullptr);
}

void MainWindow::OnRemoveFromListAccelerator(
    KeyboardAccelerator const& Key, KeyboardAcceleratorInvokedEventArgs const& args) {
    UNREFERENCED_PARAMETER(Key);
    args.Handled(true);
    if (!alive_.load()) return;

    auto sel = SelectedIndices();
    if (sel.empty()) return;
    // 从后往前删，避免下标漂移
    for (auto it = sel.rbegin(); it != sel.rend(); ++it) {
        if (*it < items_.size()) items_.erase(items_.begin() + static_cast<ptrdiff_t>(*it));
    }
    RebuildFileList();
    RebuildProcessList();
    UpdateSummary();
    RefreshEmptyHint();
    SetStatus(L"已从列表移除 " + std::to_wstring(sel.size()) + L" 项（磁盘上的文件没有改动）");
}

}  // namespace winrt::Unlocker::implementation
