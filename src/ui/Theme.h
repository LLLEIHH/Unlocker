// 主题层：唯一知道「现在是深色还是浅色」「强调色是什么」的地方。
// 其余绘制代码只引用 token 名，不出现颜色字面量（否则深色模式永远补不全）。
#pragma once

#include <windows.h>

#include <string>

namespace ul::theme {

// 外观偏好：默认跟随系统，也可以在设置里强制浅色/深色
enum class Mode { System = 0, Light, Dark };

// 语义色 token，与方案 §2.2 一一对应；末尾几个是为状态文字/悬停补充的派生色。
enum class Token {
    WindowBg = 0,   // 窗口底色（卡片之外的所有区域）
    CardBg,         // 列表卡片、控件底
    CardBorder,     // 卡片 1px 描边
    Divider,        // 表头下边线、行分隔
    TextPrimary,
    TextSecondary,
    TextTertiary,
    Accent,         // 主按钮底、选中条、焦点环
    AccentHover,
    AccentPressed,
    AccentText,
    ControlHover,
    ControlPressed,
    SelectedBg,     // 列表选中行
    Danger,
    DangerBg,
    Warning,
    WarningBg,
    Success,
    Muted,
    Count
};

struct Palette {
    COLORREF color[static_cast<int>(Token::Count)]{};
    HBRUSH   brush[static_cast<int>(Token::Count)]{};
    HFONT    body = nullptr;      // 12px 正文/列表/按钮
    HFONT    strong = nullptr;    // 13px 半粗（标题栏主标题）
    HFONT    title = nullptr;     // 14px 半粗（空态主文案）
    HFONT    caption = nullptr;   // 11px 列头/状态/副标题
    HFONT    icon = nullptr;      // 14px Fluent Icons
    HFONT    iconSmall = nullptr; // 10px Fluent Icons（窗口按钮）
    HFONT    iconLarge = nullptr; // 36px Fluent Icons（空态图示）
    int      dpi = 96;
    bool     dark = false;
    bool     highContrast = false;
};

// 一次控件绘制的入参：把 itemState 位标志在调用前归一化，绘制函数里不再判 ODS_*
struct ButtonPaint {
    RECT         rc{};
    std::wstring text;
    std::wstring glyph;          // 可选前置图标（Fluent Icons / MDL2 码位）
    bool         primary = false;
    bool         checkbox = false;
    bool         checked = false;
    bool         caption = false;    // 标题栏窗口按钮：方形、只画 glyph
    bool         closeStyle = false; // 关闭按钮：hover 用 Danger 底
    bool         link = false;       // 超链接：无底无框，强调色文字，hover 加下划线
    bool         flat = false;       // 复选框用在对话框里时不要白卡片底，直接铺窗口底色
    bool         hot = false;
    bool         pressed = false;
    bool         focused = false;
    bool         disabled = false;
};

void InitProcess();                   // 建窗口之前调：GDI+ 启动 + uxtheme 绑定
bool Init(HWND hwnd);                 // 建窗口之后调：读主题、建画刷字体
void Shutdown();
void Refresh(HWND hwnd);              // 主题 / DPI / 非客户区度量变化时重建
void ApplyWindowFrame(HWND hwnd);     // DWM 圆角 + 深色标题 + 深色滚动条
void ApplyChildTheme(HWND child);     // 子控件（列表/表头）跟随深浅色
void ClearChildTheme(HWND child);     // 关掉视觉样式（全自绘的控件用，避免系统态覆盖）
void SetThemeClass(HWND child, const wchar_t* cls);  // 指定视觉样式类（组合框等原生控件用）

Mode GetMode();                  // 当前偏好（不读取系统，只看用户选择）
void SetMode(Mode mode);         // 写注册表并重建调色板；调用方负责重绘窗口

const Palette& Get();
bool  IsDark();
bool  HighContrast();
int   Dpi();
int   Scale(int px96);                // 96 DPI 逻辑值 → 当前 DPI 像素
int   ControlPadX();                  // 控件左右内边距（150% 缩放下固定 15px）

COLORREF Color(Token t);
HBRUSH   Brush(Token t);

HFONT FontBody();
HFONT FontStrong();
HFONT FontTitle();
HFONT FontCaption();
HFONT FontIcon();
HFONT FontIconSmall();
HFONT FontIconLarge();

// 绘制原语：形状走 GDI+（抗锯齿圆角），文本走 GDI DrawTextW（ClearType 更清晰）
void FillSolid(HDC dc, const RECT& rc, COLORREF color);
void FillRound(HDC dc, const RECT& rc, int radius, COLORREF color);
void StrokeRound(HDC dc, const RECT& rc, int radius, COLORREF color, int width = 1);
void DrawCard(HDC dc, const RECT& rc, int radius);
void DrawTextLine(HDC dc, const std::wstring& text, const RECT& rc, HFONT font,
                  COLORREF color, UINT flags);
void DrawTextLine(HDC dc, const wchar_t* text, const RECT& rc, HFONT font,
                  COLORREF color, UINT flags);
int  MeasureText(HDC dc, const std::wstring& text, HFONT font);
// 单个字形的实际墨迹宽度（不含字体本身的左右边距），图标按钮用它算内边距
int  MeasureGlyphInk(HDC dc, wchar_t ch, HFONT font);
void PaintButton(HDC dc, const ButtonPaint& p);

}  // namespace ul::theme
