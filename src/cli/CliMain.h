#pragma once
#include <string>
#include <vector>

namespace ul {

// 返回进程退出码（方案 4.9）
int CliMain(int argc, wchar_t** argv);

// true 表示这组参数是「动作型」命令（应走 CLI 而不是打开 GUI）
bool CliHasActionFlag(int argc, wchar_t** argv);

bool CliHandlesInstall(int argc, wchar_t** argv);

}  // namespace ul
