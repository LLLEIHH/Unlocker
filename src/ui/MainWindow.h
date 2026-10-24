#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace ul {

int GuiMain(HINSTANCE inst, const std::vector<std::wstring>& initialPaths, int nCmdShow);

}  // namespace ul
