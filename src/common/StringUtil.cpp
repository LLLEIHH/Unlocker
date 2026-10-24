#include "common/StringUtil.h"

#include <windows.h>
#include <cwctype>
#include <cstdio>

namespace ul {

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring ToUpper(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towupper(c));
    return s;
}

bool IEquals(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                               b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool IStartsWith(const std::wstring& s, const std::wstring& prefix) {
    if (prefix.size() > s.size()) return false;
    return CompareStringOrdinal(s.c_str(), static_cast<int>(prefix.size()),
                               prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

bool IEndsWith(const std::wstring& s, const std::wstring& suffix) {
    if (suffix.size() > s.size()) return false;
    const wchar_t* tail = s.c_str() + (s.size() - suffix.size());
    return CompareStringOrdinal(tail, static_cast<int>(suffix.size()),
                               suffix.c_str(), static_cast<int>(suffix.size()), TRUE) == CSTR_EQUAL;
}

std::string Utf16ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToUtf16(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return s.substr(b, e - b);
}

std::wstring TrimRightDotsSpaces(const std::wstring& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == L' ' || s[e - 1] == L'.')) --e;
    return s.substr(0, e);
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

std::wstring NormalizeSeparators(std::wstring s) {
    for (auto& c : s) if (c == L'/') c = L'\\';
    if (s.size() > 3) {  // 保留 "C:\"
        while (!s.empty() && s.back() == L'\\') s.pop_back();
    }
    return s;
}

bool IsUncPath(const std::wstring& path) {
    return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

bool IsVolumeRoot(const std::wstring& path) {
    // 允许 "C:\" 与 "C:"
    if (path.size() == 2 && path[1] == L':') return true;
    if (path.size() == 3 && path[1] == L':' && path[2] == L'\\') return true;
    return false;
}

bool IsDevicePath(const std::wstring& path) {
    std::wstring p = ToLower(path);
    if (IStartsWith(p, L"\\\\?\\") || IStartsWith(p, L"\\??\\")) return true;
    return IStartsWith(p, L"\\device\\");
}

std::wstring FormatSize(unsigned long long bytes) {
    wchar_t buf[64];
    const double kb = 1024.0;
    if (bytes < 1024ull) swprintf_s(buf, L"%llu B", bytes);
    else if (bytes < 1024ull * 1024) swprintf_s(buf, L"%.1f KB", bytes / kb);
    else if (bytes < 1024ull * 1024 * 1024) swprintf_s(buf, L"%.1f MB", bytes / (kb * kb));
    else swprintf_s(buf, L"%.2f GB", bytes / (kb * kb * kb));
    return buf;
}

std::wstring Hex32(unsigned long v) {
    wchar_t buf[16];
    swprintf_s(buf, L"0x%08lX", v);
    return buf;
}

std::wstring MaskSecret(const std::wstring& s) {
    if (s.size() <= 8) return L"****";
    return s.substr(0, 8) + L"****";
}

}  // namespace ul
