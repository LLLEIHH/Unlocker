#pragma once
// 双语支持（中/英）。设计取舍（不引 resw/PRI，用代码内 T(zh,en)）：
//   · 不引 resw / PRI —— 本工程的 XAML 管线是手写胶水，不再给它加负载
//   · 不用巨型字符串枚举 —— 文案直接写在调用点（T(L"中文", L"English")），
//     改一处不用同时在两个文件里同步，漏翻也不会错位
#include <string>

namespace ul {

enum class Lang { Zh, En };

Lang GetLang();
void SetLang(Lang lang);

// 系统首选 UI 语言是 zh-*（含 zh-CN / zh-TW / zh-HK / zh-MO / zh-SG）→ 简体中文，否则英文
Lang DetectSystemLang();

// 读 HKCU\Software\Unlocker\Language；没有记录就用 DetectSystemLang()
Lang LoadLangPreference();
void SaveLangPreference(Lang lang);

// 单句文案
inline const wchar_t* T(const wchar_t* zh, const wchar_t* en) {
    return GetLang() == Lang::En ? en : zh;
}

// 带参数文案：中英格式串分别给（语序不同，不能共用）
std::wstring Tf(const wchar_t* zhFmt, const wchar_t* enFmt, ...);

}  // namespace ul
