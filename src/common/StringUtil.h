#pragma once
#include <string>
#include <vector>

namespace ul {

std::wstring ToLower(std::wstring s);
std::wstring ToUpper(std::wstring s);
bool IEquals(const std::wstring& a, const std::wstring& b);
bool IStartsWith(const std::wstring& s, const std::wstring& prefix);
bool IEndsWith(const std::wstring& s, const std::wstring& suffix);

std::string  Utf16ToUtf8(const std::wstring& s);
std::wstring Utf8ToUtf16(const std::string& s);

std::wstring Trim(const std::wstring& s);
std::wstring TrimRightDotsSpaces(const std::wstring& s);
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep);

// 反斜杠统一、去尾部斜杠（卷根 "C:\" 保留）
std::wstring NormalizeSeparators(std::wstring s);
bool IsUncPath(const std::wstring& path);
bool IsVolumeRoot(const std::wstring& path);          // "C:\"
bool IsDevicePath(const std::wstring& path);          // \Device\... 或 \??\...
std::wstring FormatSize(unsigned long long bytes);
std::wstring Hex32(unsigned long v);

// 日志打码：只保留前 8 个字符
std::wstring MaskSecret(const std::wstring& s);

}  // namespace ul
