#pragma once
// 启动参数里的路径：main.cpp 写入，MainWindow 构造时取走
#include <string>
#include <vector>

namespace ul {

void SetInitialPaths(std::vector<std::wstring> paths);
std::vector<std::wstring> TakeInitialPaths();

}  // namespace ul
