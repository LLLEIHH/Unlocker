// 多语言文案：所有可翻译字符串集中在 LangTable.cpp 的一张二维表里，
// 加一种语言 = 给表里每一行多补一列（编译器会检查列数），再加一个 Lang 枚举值。
// 调用点只引用 Str::Xxx，不再把文案写死在业务代码里。
#pragma once

#include <string>

namespace ul {

enum class Lang {
    ZhCN = 0,   // 简体中文
    ZhTW,       // 繁體中文（台灣）
    ZhHK,       // 繁體中文（香港）
    ZhMO,       // 繁體中文（澳門）
    En,
    Count
};

enum class Str : int {
    AppSubtitle,
    BtnAddFile,
    BtnAddFolder,
    BtnAdminScan,
    BtnForce,
    BtnUnlock,
    BtnUnlockDelete,
    BtnUnlockRename,
    BtnUnlockMove,
    BtnUnlockCopy,
    BtnRescan,
    BtnMinimize,
    BtnMaximize,
    BtnRestore,
    BtnClose,
    BtnSettings,
    ColPath,
    ColSize,
    ColOwner,
    ColStatus,
    ColProcess,
    ColPid,
    ColDesc,
    ColHandles,
    HintDrop,
    BannerFailClosed,
    BannerElevated,
    StatusIdle,
    StatusIdleElevated,
    StatusFailClosed,
    StatusScanning,
    StatusRescanning,
    StatusScanDone,
    StatusDeepRequest,
    StatusDeepProgress,
    StatusDeepFailed,
    StatusDeepDone,
    StatusOpRequest,
    StatusOpProgress,
    StatusOpRunning,
    StatusOpDone,
    StatusOpFailed,
    StatusNothingToAdd,
    StateBlocked,
    StateConfirm,
    StateConfirmPrefix,
    StateScanning,
    StatePending,
    EntryIsDir,
    OwnerChildPrefix,
    OwnerChildSuffix,
    ProcProtected,
    ProcMapped,
    ProcChildLock,
    ProcInUse,
    ProcUnknown,
    ProcSkippedMany,
    DlgTooMany,
    DlgNoTargetsDenied,
    DlgNoTargets,
    DlgElevationCancelled,
    DlgTitlePartial,
    DlgNothingToScan,
    DlgBusyQuit,
    DlgDeniedTitle,
    DlgDeniedSuffix,
    DlgDeniedFooter,
    OpSummary,
    OpRebootNote,
    OpDetailSkipped,
    OpDetailFailed,
    OpDetailRule,
    SettingsTitle,
    SettingsAppearance,
    ThemeSystem,
    ThemeLight,
    ThemeDark,
    SettingsLanguage,
    SettingsHint,
    SettingsShellMenu,
    SettingsShellMenuHint,
    ShellMenuVerb,
    ShellPromptTitle,
    ShellPromptBody,
    ShellPromptAdd,
    ShellPromptLater,
    ShellInstallFailed,
    SettingsAbout,
    SettingsLicense,
    OccupancyReady,
    OccupancyOpen,
    OccupancyLocked,
    OccupancyMapped,
    OccupancyDenied,
    OccupancyKernelLock,
    OccupancyMissing,
    OccupancyInaccessible,
    OccupancyUnknown,
    ExplainFree,
    ExplainOpen,
    ExplainLocked,
    ExplainMapped,
    ExplainDenied,
    ExplainKernelLock,
    ExplainMissing,
    ExplainError,
    ErrSuccess,
    ErrFileNotFound,
    ErrPathNotFound,
    ErrAccessDenied,
    ErrSharing,
    ErrLock,
    ErrInvalidName,
    ErrBadPath,
    ErrInvalidParam,
    ErrDirNotEmpty,
    ErrNotSameDevice,
    ErrDiskFull,
    ErrWriteProtect,
    ErrCancelled,
    ErrPrivilege,
    ErrInvalidHandle,
    ErrProcessAborted,
    ErrFileTooLarge,
    ErrNotSupported,
    ErrPathTooLong,
    ErrUnknown,
    ErrElevationCancelled,
    ErrWithCodeFmt,
    Count
};

Lang GetLang();
void SetLang(Lang lang);

// 系统首选 UI 语言 → 界面语言。zh-Hant 按地区细分到台/港/澳，其余中文用简体，非中文用英文。
Lang DetectSystemLang();

// 读 HKCU\Software\Unlocker\Language（"zh-CN" / "zh-TW" / "zh-HK" / "zh-MO" / "en"）
Lang LoadLangPreference();
void SaveLangPreference(Lang lang);

// 语言自身的名称（用于下拉框，永远用该语言自己的写法）
const wchar_t* LangNativeName(Lang lang);
// 稳定的存储标识（注册表用）
const wchar_t* LangTag(Lang lang);

const wchar_t* T(Str id);              // 当前语言
const wchar_t* T(Str id, Lang lang);   // 指定语言
std::wstring Tf(Str fmt, ...);         // 带参数（按当前语言取格式串）

}  // namespace ul
