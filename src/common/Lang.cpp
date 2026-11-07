#include "common/Lang.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace ul {
namespace {

std::atomic<Lang> g_lang{Lang::ZhCN};

constexpr wchar_t kRegKey[] = L"Software\\Unlocker";
constexpr wchar_t kRegValue[] = L"Language";

bool Contains(const std::wstring& s, const wchar_t* needle) {
    return s.find(needle) != std::wstring::npos;
}

// "zh-Hant-TW" / "zh-TW" / "zh-Hans-CN" … → 具体语言
Lang FromTag(const std::wstring& raw) {
    std::wstring tag;
    tag.reserve(raw.size());
    for (wchar_t c : raw) tag.push_back(static_cast<wchar_t>(towlower(c)));

    if (tag.size() < 2 || tag[0] != L'z' || tag[1] != L'h') return Lang::En;

    // 繁体：按地区细分。澳门沿用香港用词，但仍单独保留一项，便于以后微调。
    const bool traditional = Contains(tag, L"hant") || Contains(tag, L"cht") ||
                             Contains(tag, L"-tw") || Contains(tag, L"-hk") ||
                             Contains(tag, L"-mo");
    if (traditional) {
        if (Contains(tag, L"-hk")) return Lang::ZhHK;
        if (Contains(tag, L"-mo")) return Lang::ZhMO;
        return Lang::ZhTW;   // -TW 或只有 Hant 没有地区
    }
    // 简体：zh / zh-Hans* / zh-CN / zh-SG / zh-CHS
    return Lang::ZhCN;
}

}  // namespace

Lang GetLang() { return g_lang.load(std::memory_order_relaxed); }

void SetLang(Lang lang) { g_lang.store(lang, std::memory_order_relaxed); }

Lang DetectSystemLang() {
    // GetUserPreferredUILanguages 比 GetSystemDefaultUILanguage 更贴「用户实际看到的界面语言」
    // （用户在「语言首选项」里调整的顺序会被尊重）。名称形如 "zh-Hant-TW"、"en-US"。
    //
    // 注意：pcchLanguagesBuffer 的单位是**字符**（TCHAR），不是字节。
    // 早期实现按字节除以 sizeof(wchar_t) 折算，缓冲区只有实际需要的一半，每次调用都写越界
    // （表现为随机 0xC0000374 堆损坏）。这里按字符数分配并显式传容量。
    DWORD count = 0;
    ULONG chars = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &chars) || chars == 0) {
        return Lang::ZhCN;   // 取不到就按中文（本产品主用户群）
    }
    std::vector<wchar_t> buf(static_cast<size_t>(chars) + 4, L'\0');
    ULONG capacity = static_cast<ULONG>(buf.size());
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &capacity)) {
        return Lang::ZhCN;
    }

    // 只看第一项（首选语言）；分隔符是 \0
    return FromTag(std::wstring(buf.data()));
}

Lang LoadLangPreference() {
    Lang result = DetectSystemLang();
    wchar_t value[32]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, kRegValue, RRF_RT_REG_SZ, nullptr, value,
                     &size) == ERROR_SUCCESS && value[0] != L'\0') {
        // 用户手动选过就用它覆盖系统判断；_wcsicmp 让 "EN" / "en" 都能认
        if (_wcsicmp(value, L"en") == 0) {
            result = Lang::En;
        } else {
            result = FromTag(value);
        }
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
    const wchar_t* v = LangTag(lang);
    RegSetValueExW(key, kRegValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(v),
                   static_cast<DWORD>((wcslen(v) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

const wchar_t* LangTag(Lang lang) {
    switch (lang) {
        case Lang::ZhCN: return L"zh-CN";
        case Lang::ZhTW: return L"zh-TW";
        case Lang::ZhHK: return L"zh-HK";
        case Lang::ZhMO: return L"zh-MO";
        case Lang::En:   return L"en";
        default:         return L"zh-CN";
    }
}

// 下拉框里显示的语言名：永远用该语言自己的写法，用户不管当前是什么语言都能认出自己的母语
const wchar_t* LangNativeName(Lang lang) {
    switch (lang) {
        case Lang::ZhCN: return L"简体中文";
        case Lang::ZhTW: return L"繁體中文（台灣）";
        case Lang::ZhHK: return L"繁體中文（香港）";
        case Lang::ZhMO: return L"繁體中文（澳門）";
        case Lang::En:   return L"English";
        default:         return L"English";
    }
}

std::wstring Tf(Str fmt, ...) {
    const wchar_t* format = T(fmt);
    if (!format || !*format) return std::wstring();

    wchar_t buf[1024]{};
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnwprintf_s(buf, _TRUNCATE, format, ap);
    va_end(ap);
    if (n <= 0) return std::wstring();
    return std::wstring(buf, static_cast<size_t>(n));
}

#ifdef UNLOCKER_LEGACY_LANG_API
const wchar_t* T(const wchar_t* zh, const wchar_t* en) {
    return GetLang() == Lang::En ? en : zh;
}

std::wstring Tf(const wchar_t* zh, const wchar_t* en, ...) {
    const wchar_t* format = T(zh, en);
    if (!format || !*format) return std::wstring();

    wchar_t buf[1024]{};
    va_list ap;
    va_start(ap, en);
    const int n = _vsnwprintf_s(buf, _TRUNCATE, format, ap);
    va_end(ap);
    if (n <= 0) return std::wstring();
    return std::wstring(buf, static_cast<size_t>(n));
}
#endif

}  // namespace ul
