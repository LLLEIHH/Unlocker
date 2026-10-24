#pragma once
// 通道 A：Shell 扩展 / 第二个实例 -> UI，中完整性到中完整性，不走 UAC。
// 只传路径，不做任何危险动作；与 worker 的高危通道（通道 B）严格分开。
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace ul {

constexpr UINT WM_APP_SHELL_PATHS = WM_APP + 5;

std::wstring UiPipeName();

// 在 timeoutMs 预算内重试连接（首实例可能还在启动，管道尚未创建，见 R-27）
bool SendPathsToRunningInstance(const std::vector<std::wstring>& paths, DWORD timeoutMs = 3000);

// 在独立线程里监听；收到路径后 PostMessage(hwnd, WM_APP_SHELL_PATHS, 0, new std::vector<std::wstring>)
bool StartUiPipeListener(HWND hwnd);

// WinUI 宿主没有自己的 WndProc，用回调版本（回调会在监听线程里调用，调用方自行切线程）
bool StartUiPipeListenerFn(std::function<void(std::vector<std::wstring>)> callback);

}  // namespace ul
