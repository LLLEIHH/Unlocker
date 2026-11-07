#include "ui/Theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include <objidl.h>
// gdiplus.h 在 NOMINMAX 下会用到 min/max，必须先引入 std 版本（全局只在 .cpp 里污染）
using std::max;
using std::min;
#include <gdiplus.h>

namespace ul::theme {
namespace {

// ---------------------------------------------------------------- 内部状态
struct State {
    Palette  pal;
    HWND     hwnd = nullptr;
    ULONG_PTR gdiplusToken = 0;
    bool     initialized = false;
    Mode     mode = Mode::System;   // 用户偏好（注册表），System 时才看系统设置

    HMODULE uxtheme = nullptr;
    int  (WINAPI* setPreferredAppMode)(int) = nullptr;
    BOOL (WINAPI* allowDarkModeForWindow)(HWND, BOOL) = nullptr;
    void (WINAPI* flushMenuThemes)() = nullptr;

    std::unordered_map<COLORREF, HBRUSH> solidBrushes;
};

State s;



COLORREF Blend(COLORREF a, COLORREF b, int percentB) {
    const int p = std::clamp(percentB, 0, 100);
    const auto mix = [p](int x, int y) { return x + (y - x) * p / 100; };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
               mix(GetBValue(a), GetBValue(b)));
}

double ChannelLuminance(double v) {
    v /= 255.0;
    return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double Luminance(COLORREF c) {
    return 0.2126 * ChannelLuminance(GetRValue(c)) + 0.7152 * ChannelLuminance(GetGValue(c)) +
           0.0722 * ChannelLuminance(GetBValue(c));
}

double Contrast(COLORREF a, COLORREF b) {
    const double la = Luminance(a), lb = Luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// ---------------------------------------------------------------- 外观偏好
constexpr wchar_t kThemeKey[]   = L"Software\\Unlocker";
constexpr wchar_t kThemeValue[] = L"Theme";

Mode LoadModePreference() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kThemeKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return Mode::System;
    }
    wchar_t buf[32]{};
    DWORD bytes = sizeof(buf), type = 0;
    const LONG rc = RegQueryValueExW(key, kThemeValue, nullptr, &type,
                                     reinterpret_cast<BYTE*>(buf), &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return Mode::System;

    const std::wstring v = buf;
    if (_wcsicmp(v.c_str(), L"light") == 0) return Mode::Light;
    if (_wcsicmp(v.c_str(), L"dark") == 0) return Mode::Dark;
    return Mode::System;
}

void SaveModePreference(Mode mode) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kThemeKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    const wchar_t* v = mode == Mode::Light ? L"light" : (mode == Mode::Dark ? L"dark" : L"system");
    RegSetValueExW(key, kThemeValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(v),
                   static_cast<DWORD>((wcslen(v) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

// ---------------------------------------------------------------- 系统主题读取
bool SystemUsesLightTheme() {
    HKEY key = nullptr;
    DWORD value = 1, size = sizeof(value), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&value), &size) != ERROR_SUCCESS ||
            type != REG_DWORD) {
            value = 1;   // 读失败按浅色处理
        }
        RegCloseKey(key);
    }
    return value != 0;
}

bool SystemHighContrast() {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }
    return false;
}

// HKCU\...\DWM\AccentColor 是 ABGR（0xAABBGGRR），不是 ARGB——直接当 ARGB 用会拿到蓝分量
COLORREF SystemAccent() {
    HKEY key = nullptr;
    DWORD value = 0, size = sizeof(value), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", 0, KEY_QUERY_VALUE,
                      &key) == ERROR_SUCCESS) {
        const LSTATUS st = RegQueryValueExW(key, L"AccentColor", nullptr, &type,
                                            reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(key);
        if (st == ERROR_SUCCESS && type == REG_DWORD) {
            return RGB(value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF);
        }
    }
    return CLR_INVALID;
}

// 强调色可能过亮/过暗：按需往黑或往白推，直到对底色达到 4.5:1
COLORREF AccessibleAccent(COLORREF accent, COLORREF background, bool dark) {
    if (accent == CLR_INVALID) return accent;
    COLORREF c = accent;
    for (int i = 0; i < 24 && Contrast(c, background) < 4.5; ++i) {
        c = dark ? Blend(c, RGB(255, 255, 255), 8) : Blend(c, RGB(0, 0, 0), 8);
    }
    return c;
}

// ---------------------------------------------------------------- 字体
LOGFONTW BaseUiFont() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    LOGFONTW lf{};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) lf = ncm.lfMessageFont;
    if (lf.lfFaceName[0] == 0) wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return lf;
}

int CALLBACK FirstFontProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param) {
    *reinterpret_cast<bool*>(param) = true;
    return 0;   // 只要存在性，找到第一个就停
}

bool FontExists(const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    HDC dc = GetDC(nullptr);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, FirstFontProc, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

const wchar_t* IconFontFace() {
    // Win11 22000+ 有 Segoe Fluent Icons，Win10 退到 Segoe MDL2 Assets（码位兼容）
    if (FontExists(L"Segoe Fluent Icons")) return L"Segoe Fluent Icons";
    if (FontExists(L"Segoe MDL2 Assets")) return L"Segoe MDL2 Assets";
    return L"Segoe UI";
}

HFONT MakeFont(const LOGFONTW& base, int px, LONG weight, const wchar_t* face) {
    LOGFONTW lf = base;
    lf.lfHeight = -MulDiv(px, s.pal.dpi, 96);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfItalic = FALSE;
    lf.lfUnderline = FALSE;
    lf.lfStrikeOut = FALSE;
    if (face) wcscpy_s(lf.lfFaceName, face);
    return CreateFontIndirectW(&lf);
}

void DestroyPalette() {
    for (int i = 0; i < static_cast<int>(Token::Count); ++i) {
        if (s.pal.brush[i]) DeleteObject(s.pal.brush[i]);
        s.pal.brush[i] = nullptr;
    }
    for (HFONT* f : { &s.pal.body, &s.pal.strong, &s.pal.title, &s.pal.caption, &s.pal.icon,
                      &s.pal.iconSmall, &s.pal.iconLarge }) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

void SetColor(Token t, COLORREF c) {
    s.pal.color[static_cast<int>(t)] = c;
}

// ---------------------------------------------------------------- 调色板
void BuildPalette() {
    DestroyPalette();
    const bool systemDark = !SystemUsesLightTheme();
    s.pal.dark = s.mode == Mode::Dark || (s.mode == Mode::System && systemDark);
    s.pal.highContrast = SystemHighContrast();

    if (s.pal.highContrast) {
        // 高对比度：整体退回系统色，关闭强调色推导（无障碍审计的硬要求）
        const COLORREF bg = GetSysColor(COLOR_WINDOW);
        const COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
        const COLORREF hi = GetSysColor(COLOR_HIGHLIGHT);
        SetColor(Token::WindowBg, bg);
        SetColor(Token::CardBg, bg);
        SetColor(Token::CardBorder, fg);
        SetColor(Token::Divider, fg);
        SetColor(Token::TextPrimary, fg);
        SetColor(Token::TextSecondary, fg);
        SetColor(Token::TextTertiary, fg);
        SetColor(Token::Accent, hi);
        SetColor(Token::AccentHover, hi);
        SetColor(Token::AccentPressed, hi);
        SetColor(Token::AccentText, GetSysColor(COLOR_HIGHLIGHTTEXT));
        SetColor(Token::ControlHover, bg);
        SetColor(Token::ControlPressed, bg);
        SetColor(Token::SelectedBg, hi);
        SetColor(Token::Danger, fg);
        SetColor(Token::DangerBg, bg);
        SetColor(Token::Warning, fg);
        SetColor(Token::WarningBg, bg);
        SetColor(Token::Success, fg);
        SetColor(Token::Muted, fg);
    } else if (s.pal.dark) {
        SetColor(Token::WindowBg, RGB(0x20, 0x20, 0x20));
        SetColor(Token::CardBg, RGB(0x2B, 0x2B, 0x2B));
        SetColor(Token::CardBorder, RGB(0x3A, 0x3A, 0x3A));
        SetColor(Token::Divider, RGB(0x3A, 0x3A, 0x3A));
        SetColor(Token::TextPrimary, RGB(0xFF, 0xFF, 0xFF));
        SetColor(Token::TextSecondary, RGB(0xA0, 0xA0, 0xA0));
        SetColor(Token::TextTertiary, RGB(0x7A, 0x7A, 0x7A));
        SetColor(Token::Accent, RGB(0x60, 0xCD, 0xFF));
        SetColor(Token::AccentText, RGB(0x00, 0x00, 0x00));
        SetColor(Token::ControlHover, RGB(0x33, 0x33, 0x33));
        SetColor(Token::ControlPressed, RGB(0x3A, 0x3A, 0x3A));
        SetColor(Token::SelectedBg, RGB(0x14, 0x38, 0x4F));
        SetColor(Token::Danger, RGB(0xFF, 0x99, 0xA4));
        SetColor(Token::DangerBg, RGB(0x44, 0x27, 0x26));
        SetColor(Token::Warning, RGB(0xFC, 0xE1, 0x00));
        SetColor(Token::WarningBg, RGB(0x43, 0x35, 0x19));
        SetColor(Token::Success, RGB(0x6C, 0xCB, 0x5F));
        SetColor(Token::Muted, RGB(0xA0, 0xA0, 0xA0));
    } else {
        SetColor(Token::WindowBg, RGB(0xF3, 0xF3, 0xF3));
        SetColor(Token::CardBg, RGB(0xFF, 0xFF, 0xFF));
        SetColor(Token::CardBorder, RGB(0xE5, 0xE5, 0xE5));
        SetColor(Token::Divider, RGB(0xEB, 0xEB, 0xEB));
        SetColor(Token::TextPrimary, RGB(0x1B, 0x1B, 0x1B));
        SetColor(Token::TextSecondary, RGB(0x5D, 0x5D, 0x5D));
        SetColor(Token::TextTertiary, RGB(0x8A, 0x8A, 0x8A));
        SetColor(Token::Accent, RGB(0x00, 0x5F, 0xB8));
        SetColor(Token::AccentText, RGB(0xFF, 0xFF, 0xFF));
        SetColor(Token::ControlHover, RGB(0xF9, 0xF9, 0xF9));
        SetColor(Token::ControlPressed, RGB(0xF0, 0xF0, 0xF0));
        SetColor(Token::SelectedBg, RGB(0xE8, 0xF1, 0xFA));
        SetColor(Token::Danger, RGB(0xC4, 0x2B, 0x1C));
        SetColor(Token::DangerBg, RGB(0xFD, 0xE7, 0xE9));
        SetColor(Token::Warning, RGB(0x9D, 0x5D, 0x00));
        SetColor(Token::WarningBg, RGB(0xFF, 0xF4, 0xCE));
        SetColor(Token::Success, RGB(0x0F, 0x7B, 0x0F));
        SetColor(Token::Muted, RGB(0x8A, 0x8A, 0x8A));
    }

    if (!s.pal.highContrast) {
        // 系统强调色：失败就用 token 表里的 WinUI 基准值（比运行时算出来的更稳）
        const COLORREF fallback = s.pal.color[static_cast<int>(Token::Accent)];
        COLORREF accent = SystemAccent();
        if (accent == CLR_INVALID) accent = fallback;
        accent = AccessibleAccent(accent, s.pal.color[static_cast<int>(Token::WindowBg)], s.pal.dark);
        SetColor(Token::Accent, accent);
        SetColor(Token::AccentHover, Blend(accent, RGB(255, 255, 255), 12));
        SetColor(Token::AccentPressed, Blend(accent, RGB(0, 0, 0), 12));
        // 选中行 = 强调色 12% 混卡片底
        SetColor(Token::SelectedBg,
                 Blend(s.pal.color[static_cast<int>(Token::CardBg)], accent, 14));
    }

    for (int i = 0; i < static_cast<int>(Token::Count); ++i) {
        s.pal.brush[i] = CreateSolidBrush(s.pal.color[i]);
    }

    const LOGFONTW base = BaseUiFont();
    const wchar_t* iconFace = IconFontFace();
    s.pal.body = MakeFont(base, 12, base.lfWeight, nullptr);
    s.pal.strong = MakeFont(base, 13, FW_SEMIBOLD, nullptr);
    s.pal.title = MakeFont(base, 14, FW_SEMIBOLD, nullptr);
    s.pal.caption = MakeFont(base, 11, base.lfWeight, nullptr);
    s.pal.icon = MakeFont(base, 14, base.lfWeight, iconFace);
    s.pal.iconSmall = MakeFont(base, 10, base.lfWeight, iconFace);
    s.pal.iconLarge = MakeFont(base, 36, base.lfWeight, iconFace);

    // 进程级深色偏好必须在第一个窗口创建前设好；之后每次切换再补一次
    if (s.setPreferredAppMode) s.setPreferredAppMode(s.pal.dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
}

// ---------------------------------------------------------------- GDI+ 路径
Gdiplus::Color ToGdi(COLORREF c) {
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

void RoundPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    if (radius <= 0.0f) {
        path.AddRectangle(r);
        return;
    }
    const float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

HBRUSH SolidBrush(COLORREF c) {
    auto it = s.solidBrushes.find(c);
    if (it != s.solidBrushes.end()) return it->second;
    HBRUSH b = CreateSolidBrush(c);
    s.solidBrushes.emplace(c, b);
    return b;
}

}  // namespace

// ---------------------------------------------------------------- 生命周期
void InitProcess() {
    if (s.gdiplusToken == 0) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&s.gdiplusToken, &input, nullptr);
    }
    if (!s.uxtheme) {
        s.uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (s.uxtheme) {
            // 未文档化序号：133/135/136，全部判空，取不到就退化成系统默认外观
            s.setPreferredAppMode = reinterpret_cast<int(WINAPI*)(int)>(
                GetProcAddress(s.uxtheme, MAKEINTRESOURCEA(135)));
            s.allowDarkModeForWindow = reinterpret_cast<BOOL(WINAPI*)(HWND, BOOL)>(
                GetProcAddress(s.uxtheme, MAKEINTRESOURCEA(133)));
            s.flushMenuThemes = reinterpret_cast<void(WINAPI*)()>(
                GetProcAddress(s.uxtheme, MAKEINTRESOURCEA(136)));
        }
    }
    s.pal.dpi = 96;
    s.mode = LoadModePreference();
    BuildPalette();
}

bool Init(HWND hwnd) {
    s.hwnd = hwnd;
    if (!s.gdiplusToken) InitProcess();
    s.pal.dpi = hwnd ? static_cast<int>(GetDpiForWindow(hwnd)) : 96;
    if (s.pal.dpi <= 0) s.pal.dpi = 96;
    BuildPalette();
    ApplyWindowFrame(hwnd);
    s.initialized = true;
    return true;
}

void Shutdown() {
    DestroyPalette();
    for (auto& kv : s.solidBrushes) DeleteObject(kv.second);
    s.solidBrushes.clear();
    if (s.flushMenuThemes) s.flushMenuThemes();
    if (s.uxtheme) {
        FreeLibrary(s.uxtheme);
        s.uxtheme = nullptr;
    }
    if (s.gdiplusToken) {
        Gdiplus::GdiplusShutdown(s.gdiplusToken);
        s.gdiplusToken = 0;
    }
    s.initialized = false;
}

void Refresh(HWND hwnd) {
    s.hwnd = hwnd;
    s.pal.dpi = hwnd ? static_cast<int>(GetDpiForWindow(hwnd)) : 96;
    if (s.pal.dpi <= 0) s.pal.dpi = 96;
    BuildPalette();
    for (auto& kv : s.solidBrushes) DeleteObject(kv.second);
    s.solidBrushes.clear();
    ApplyWindowFrame(hwnd);
}

void ApplyWindowFrame(HWND hwnd) {
    if (!hwnd) return;
    const BOOL dark = s.pal.dark ? TRUE : FALSE;
    // 20 是 Win10 2004+ 的 DWMWA_USE_IMMERSIVE_DARK_MODE，1809 用 19
    if (FAILED(DwmSetWindowAttribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(19), &dark, sizeof(dark));
    }
    // DWMWA_WINDOW_CORNER_PREFERENCE = 33 / DWMWCP_ROUND = 2；Win11 22000 以下返回 E_INVALIDARG
    const int corner = s.pal.highContrast ? 1 /*DONOTROUND*/ : 2 /*ROUND*/;
    DwmSetWindowAttribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(33), &corner, sizeof(corner));
    if (s.allowDarkModeForWindow) s.allowDarkModeForWindow(hwnd, dark);
}

void ApplyChildTheme(HWND child) {
    if (!child) return;
    if (s.allowDarkModeForWindow) s.allowDarkModeForWindow(child, s.pal.dark ? TRUE : FALSE);
    SetWindowTheme(child, s.pal.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

void SetThemeClass(HWND child, const wchar_t* cls) {
    if (!child) return;
    SetWindowTheme(child, cls, nullptr);
}

void ClearChildTheme(HWND child) {
    if (!child) return;
    if (s.allowDarkModeForWindow) s.allowDarkModeForWindow(child, s.pal.dark ? TRUE : FALSE);
    SetWindowTheme(child, L"", L"");
}

// ---------------------------------------------------------------- 查询
const Palette& Get() { return s.pal; }
bool IsDark() { return s.pal.dark; }

Mode GetMode() { return s.mode; }

void SetMode(Mode mode) {
    if (s.mode == mode) return;
    s.mode = mode;
    SaveModePreference(mode);
    BuildPalette();
    if (s.flushMenuThemes) s.flushMenuThemes();   // 已打开的菜单/下拉重新取色
}
bool HighContrast() { return s.pal.highContrast; }
int Dpi() { return s.pal.dpi > 0 ? s.pal.dpi : 96; }
int Scale(int px96) { return MulDiv(px96, Dpi(), 96); }
// 控件左右内边距：统一取 10 逻辑px —— 150% 缩放下正好 15px，且随 DPI 等比放大

int ControlPadX() { return Scale(10); }


COLORREF Color(Token t) { return s.pal.color[static_cast<int>(t)]; }
HBRUSH Brush(Token t) { return s.pal.brush[static_cast<int>(t)]; }

HFONT FontBody() { return s.pal.body; }
HFONT FontStrong() { return s.pal.strong; }
HFONT FontTitle() { return s.pal.title; }
HFONT FontCaption() { return s.pal.caption; }
HFONT FontIcon() { return s.pal.icon; }
HFONT FontIconSmall() { return s.pal.iconSmall; }
HFONT FontIconLarge() { return s.pal.iconLarge; }

// ---------------------------------------------------------------- 绘制
void FillSolid(HDC dc, const RECT& rc, COLORREF color) {
    ::FillRect(dc, &rc, SolidBrush(color));
}

void FillRound(HDC dc, const RECT& rc, int radius, COLORREF color) {
    if (radius <= 0) {
        FillSolid(dc, rc, color);
        return;
    }
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::GraphicsPath path;
    RoundPath(path,
              Gdiplus::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                             static_cast<float>(rc.right - rc.left),
                             static_cast<float>(rc.bottom - rc.top)),
              static_cast<float>(radius));
    Gdiplus::SolidBrush brush(ToGdi(color));
    g.FillPath(&brush, &path);
}

void StrokeRound(HDC dc, const RECT& rc, int radius, COLORREF color, int width) {
    if (rc.right - rc.left < width * 2 || rc.bottom - rc.top < width * 2) return;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float inset = width * 0.5f;
    Gdiplus::GraphicsPath path;
    RoundPath(path,
              Gdiplus::RectF(static_cast<float>(rc.left) + inset,
                             static_cast<float>(rc.top) + inset,
                             static_cast<float>(rc.right - rc.left) - width,
                             static_cast<float>(rc.bottom - rc.top) - width),
              std::max(0.0f, static_cast<float>(radius) - inset));
    Gdiplus::Pen pen(ToGdi(color), static_cast<float>(width));
    g.DrawPath(&pen, &path);
}

void DrawCard(HDC dc, const RECT& rc, int radius) {
    FillRound(dc, rc, radius, Color(Token::CardBg));
    StrokeRound(dc, rc, radius, Color(Token::CardBorder), 1);
}

void DrawTextLine(HDC dc, const wchar_t* text, const RECT& rc, HFONT font, COLORREF color,
                  UINT flags) {
    if (!text || !*text || !font) return;
    HGDIOBJ oldFont = SelectObject(dc, font);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, color);
    RECT r = rc;
    DrawTextW(dc, text, -1, &r, flags);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

void DrawTextLine(HDC dc, const std::wstring& text, const RECT& rc, HFONT font, COLORREF color,
                  UINT flags) {
    DrawTextLine(dc, text.c_str(), rc, font, color, flags);
}

int MeasureText(HDC dc, const std::wstring& text, HFONT font) {
    if (text.empty() || !font) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

int MeasureGlyphInk(HDC dc, wchar_t ch, HFONT font) {
    if (!dc || !font) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    MAT2 mat{};
    mat.eM11.value = 1;
    mat.eM22.value = 1;
    GLYPHMETRICS gm{};
    const DWORD got = GetGlyphOutlineW(dc, static_cast<UINT>(ch), GGO_METRICS, &gm, 0, nullptr, &mat);
    SelectObject(dc, old);
    if (got == GDI_ERROR) return 0;
    return static_cast<int>(gm.gmBlackBoxX);
}

void PaintButton(HDC dc, const ButtonPaint& p) {
    if (!dc) return;
    const int radius = p.caption ? 0 : Scale(4);

    // 超链接：没有底和框，只画强调色文字；悬停/按下加下划线（WinUI 的 LinkButton 观感）
    if (p.link) {
        FillSolid(dc, p.rc, Color(Token::WindowBg));
        const COLORREF linkColor = p.disabled ? Color(Token::TextTertiary) : Color(Token::Accent);
        const int textW = MeasureText(dc, p.text, FontBody());
        DrawTextLine(dc, p.text, p.rc, FontBody(), linkColor,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (p.hot || p.pressed) {
            // DT_UNDERLINE 在 GDI 的 DrawText 里不受支持，自己画一条贴着基线的细线
            const int cy = (p.rc.top + p.rc.bottom) / 2;
            RECT ul{ p.rc.left, cy + Scale(6), p.rc.left + textW, cy + Scale(7) };
            FillSolid(dc, ul, linkColor);
        }
        if (p.focused) {
            RECT fr{ p.rc.left - Scale(3), p.rc.top + Scale(1),
                     p.rc.left + textW + Scale(3), p.rc.bottom - Scale(1) };
            StrokeRound(dc, fr, Scale(3), Color(Token::Accent), 1);
        }
        return;
    }

    // 圆角外的四个角不会被 FillRound 覆盖，先按窗口底色铺满，否则移动/切主题后留下残影
    FillSolid(dc, p.rc, Color(Token::WindowBg));
    COLORREF bg = Color(Token::CardBg);
    COLORREF border = Color(Token::CardBorder);
    COLORREF fg = Color(Token::TextPrimary);

    if (p.caption) {
        // 标题栏窗口按钮：常驻透明底，hover 才有底色（Win11 的观感）
        bg = Color(Token::WindowBg);
        if (p.closeStyle && (p.hot || p.pressed)) {
            bg = p.pressed ? Blend(Color(Token::Danger), RGB(0, 0, 0), 15) : Color(Token::Danger);
            fg = RGB(255, 255, 255);
        } else if (p.pressed) {
            bg = Color(Token::ControlPressed);
        } else if (p.hot) {
            bg = Color(Token::ControlHover);
        }
        border = bg;
    } else if (p.primary) {
        bg = Color(Token::Accent);
        border = bg;
        fg = Color(Token::AccentText);
        if (p.hot) {
            bg = Color(Token::AccentHover);
            border = bg;
        }
        if (p.pressed) {
            bg = Color(Token::AccentPressed);
            border = bg;
        }
        if (p.disabled) {
            bg = Blend(Color(Token::Accent), Color(Token::WindowBg), 58);
            border = bg;
            fg = Color(Token::TextTertiary);
        }
    } else {
        if (p.hot) bg = Color(Token::ControlHover);
        if (p.pressed) bg = Color(Token::ControlPressed);
        if (p.disabled) {
            bg = Color(Token::WindowBg);
            border = Blend(Color(Token::CardBorder), Color(Token::WindowBg), 45);
            fg = Color(Token::TextTertiary);
        }
    }

    if (p.checkbox && p.flat) {   // 对话框里的复选框：不要白卡片底，和窗口融在一起
        bg = Color(Token::WindowBg);
        border = bg;
        fg = p.disabled ? Color(Token::TextTertiary) : Color(Token::TextPrimary);
    }

    FillRound(dc, p.rc, radius, bg);
    if (border != bg) StrokeRound(dc, p.rc, radius, border, 1);

    const int height = p.rc.bottom - p.rc.top;

    if (p.caption) {
        RECT gr = p.rc;
        DrawTextLine(dc, p.glyph.c_str(), gr, FontIconSmall(), fg,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (p.focused) StrokeRound(dc, p.rc, radius, Color(Token::Accent), 2);
        return;
    }

    if (p.checkbox) {
        const int box = Scale(16);
        RECT boxRc{ p.rc.left + ControlPadX(), p.rc.top + (height - box) / 2,
                    p.rc.left + ControlPadX() + box, p.rc.top + (height + box) / 2 };
        const COLORREF boxBg = p.checked ? Color(Token::Accent) : Color(Token::CardBg);
        const COLORREF boxBorder = p.checked ? Color(Token::Accent) : Color(Token::CardBorder);
        FillRound(dc, boxRc, Scale(4), boxBg);
        StrokeRound(dc, boxRc, Scale(4), p.hot ? Color(Token::Accent) : boxBorder, 1);
        if (p.checked) {
            DrawTextLine(dc, L"\xE73E", boxRc, FontIconSmall(),
                         p.disabled ? Color(Token::TextTertiary) : Color(Token::AccentText),
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        RECT tr{ boxRc.right + Scale(8), p.rc.top, p.rc.right - Scale(4), p.rc.bottom };
        DrawTextLine(dc, p.text, tr, FontBody(), p.disabled ? Color(Token::TextTertiary) : fg,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (p.focused) {
            RECT fr = p.rc;
            InflateRect(&fr, -Scale(1), -Scale(1));
            StrokeRound(dc, fr, Scale(4), Color(Token::Accent), 1);
        }
        return;
    }

    // 纯图标按钮（齿轮等）：按钮宽 = 左右内边距 + 图标宽，
    // 直接按中心绘制，字形自带的左右边距不会把留白吃成一边多一边少
    if (p.text.empty()) {
        if (!p.glyph.empty()) {
            RECT gr = p.rc;
            DrawTextLine(dc, p.glyph, gr, FontIcon(), fg,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (p.focused) {
            RECT fr = p.rc;
            InflateRect(&fr, -Scale(2), -Scale(2));
            StrokeRound(dc, fr, std::max(0, radius - Scale(1)), Color(Token::Accent), 2);
        }
        return;
    }

    // 图标 + 文本作为一个整体水平居中
    const int gap = Scale(8);
    int glyphWidth = 0;
    if (!p.glyph.empty()) glyphWidth = MeasureText(dc, p.glyph, FontIcon()) + gap;
    const int textWidth = MeasureText(dc, p.text, FontBody());
    int x = p.rc.left + ((p.rc.right - p.rc.left) - (glyphWidth + textWidth)) / 2;
    if (x < p.rc.left + Scale(6)) x = p.rc.left + Scale(6);

    if (glyphWidth > 0) {
        RECT gr{ x, p.rc.top, x + glyphWidth - gap, p.rc.bottom };
        DrawTextLine(dc, p.glyph, gr, FontIcon(), fg,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        x += glyphWidth;
    }
    RECT tr{ x, p.rc.top, p.rc.right - Scale(6), p.rc.bottom };
    DrawTextLine(dc, p.text, tr, FontBody(), fg,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (p.focused) {
        RECT fr = p.rc;
        InflateRect(&fr, -Scale(2), -Scale(2));
        StrokeRound(dc, fr, std::max(0, radius - Scale(1)),
                    p.primary ? Color(Token::AccentText) : Color(Token::Accent), 2);
    }
}

}  // namespace ul::theme
