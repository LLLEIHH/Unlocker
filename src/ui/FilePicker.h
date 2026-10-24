#pragma once
// IFileOpenDialog 封装（F17）。文件和文件夹分成两个入口：
// IFileOpenDialog 不支持混合选择，硬做只会得到「选了文件夹却返回文件」的诡异行为。
#include <windows.h>
#include <string>
#include <vector>

namespace ul {

std::vector<std::wstring> PickFiles(HWND owner);
std::vector<std::wstring> PickFolders(HWND owner);

// 生成新文件名（用于解锁并改名），返回完整路径
std::wstring PickNewName(HWND owner, const std::wstring& currentPath);

}  // namespace ul
