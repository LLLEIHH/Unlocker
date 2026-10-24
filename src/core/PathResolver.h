#pragma once
// 路径归一化（方案 4.3 / 4.6.4）。
// 这是整个项目最容易出 bug 的地方：\Device\HarddiskVolume3\Users\... 与 C:\Users\... 是同一个文件。
#include <windows.h>
#include <string>
#include <vector>

namespace ul {

class PathResolver {
public:
    static bool Init();

    // 文本归一化：剥 \\?\ 与 \??\、设备路径→DOS、GetFullPathName、GetLongPathName、
    // 折叠 . 与 ..、去每段尾随空格与点。返回原样大小写（回显用）。
    static std::wstring Normalize(const std::wstring& raw);

    // 比较用：Normalize + 大写
    static std::wstring Key(const std::wstring& raw);
    static bool PathEqual(const std::wstring& a, const std::wstring& b);

    // 句柄真身：解开 junction / symlink / 8.3 短名，得到 \\?\ 形式；返回已剥前缀的 DOS 路径。
    static std::wstring CanonicalFromHandle(HANDLE h);

    // 对尚未打开的路径求真身。
    // resolved=false 表示打不开（被独占等），此时退化为文本归一化 + 手工解 junction，
    // 调用方必须按 4.6.4 把裁决收紧到 Confirm。
    static std::wstring CanonicalPath(const std::wstring& raw, bool* resolved);

    static std::wstring DeviceToDos(const std::wstring& devicePath);
    static std::wstring DosToDevice(const std::wstring& dosPath);

    static std::wstring SelfExePath();
    static std::wstring SelfDir();

    // 手工逐段解析 junction / symlink（打不开目标时的退路）
    static std::wstring ResolveReparseLoops(const std::wstring& path, int maxDepth = 16);

    // 扫描器比对用的一组候选键（大写）：真身、文本归一化、设备形式、短名形式
    static std::vector<std::wstring> ComparisonKeys(const std::wstring& path);

    // 只做字符串层面的重解析点解析，不访问磁盘（用于已拿到的句柄名）
    static bool HasReparsePrefix(const std::wstring& p);

private:
    static std::wstring StripPrefix(std::wstring p);
    static std::wstring CollapseDots(const std::wstring& p);
    static std::wstring GetLongPathBestEffort(const std::wstring& p);
};

}  // namespace ul
