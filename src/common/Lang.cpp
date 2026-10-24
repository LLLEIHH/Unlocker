#include "common/Lang.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace ul {
namespace {

std::atomic<Lang> g_lang{Lang::Zh};

constexpr wchar_t kRegKey[] = L"Software\\Unlocker";
constexpr wchar_t kRegValue[] = L"Language";

}  // namespace

Lang GetLang() { return g_lang.load(std::memory_order_relaxed); }

void SetLang(Lang lang) { g_lang.store(lang, std::memory_order_relaxed); }

Lang DetectSystemLang() {
    // GetUserPreferredUILanguages 比 GetSystemDefaultUILanguage 更贴「用户实际看到的界面语言」
    // （用户在「语言首选项」里调整的顺序会被尊重）。名称形如 "zh-Hans-CN"、"en-US"。
    DWORD count = 0;
    DWORD bytes = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &bytes) || bytes == 0) {
        return Lang::Zh;  // 取不到就按中文（本产品主用户群）
    }
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &bytes)) {
        return Lang::Zh;
    }

    // 只看第一项（首选语言）；首个语言名以 "zh" 开头即中文。
    // zh-CN / zh-SG → 简体；zh-TW / zh-HK / zh-MO → 按产品要求也统一简体。
    const wchar_t* first = buf.data();
    if (first[0] == L'\0') return Lang::Zh;
    if ((first[0] == L'z' || first[0] == L'Z') && (first[1] == L'h' || first[1] == L'H')) {
        return Lang::Zh;
    }
    return Lang::En;
}

Lang LoadLangPreference() {
    Lang result = DetectSystemLang();
    wchar_t value[16]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, kRegValue, RRF_RT_REG_SZ, nullptr, value,
                     &size) == ERROR_SUCCESS) {
        if (_wcsicmp(value, L"en") == 0) result = Lang::En;
        else if (_wcsicmp(value, L"zh") == 0) result = Lang::Zh;
    }
    // 注意：必须写回全局状态 —— 只 return 不 SetLang 的话 GetLang() 仍是默认值
    SetLang(result);
    return result;
}

void SaveLangPreference(Lang lang) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    const wchar_t* v = (lang == Lang::En) ? L"en" : L"zh";
    RegSetValueExW(key, kRegValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(v),
                   static_cast<DWORD>((wcslen(v) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

std::wstring Tf(const wchar_t* zhFmt, const wchar_t* enFmt, ...) {
    const wchar_t* fmt = (GetLang() == Lang::En && enFmt) ? enFmt : zhFmt;
    wchar_t buf[1024]{};
    va_list ap;
    va_start(ap, enFmt);
    const int n = _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n <= 0) return std::wstring();
    return std::wstring(buf, static_cast<size_t>(n));
}

}  // namespace ul
