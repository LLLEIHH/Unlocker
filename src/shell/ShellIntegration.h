// 资源管理器右键菜单（路线 B：纯 HKCU 注册表 verb）。
//
// 为什么不用原来的 COM 扩展：
//   - COM 扩展要注册到 HKCR（需要管理员），DLL 必须和 exe 同目录，单文件版用不了；
//   - 它会被加载进 explorer 进程，稳定性和杀软误报都要额外承担；
//   - 需求只是一条固定菜单，用不上 COM 的动态能力。
//
// 覆盖范围：所有文件(*) / 文件夹(Directory) / 文件夹空白处(Directory\Background)。
// 全部写在 HKCU\Software\Classes 下，不需要管理员；卸载就是删这三个键。
#pragma once

#include <string>

namespace ul::shell {

// 当前界面语言下的菜单文字（安装时写进注册表；切语言后会重写）
std::wstring MenuText();

bool KeyExists();      // 三个入口里任意一个存在（含指向旧路径的情况）
bool IsInstalled();    // 三个入口都存在，且 command 指向当前 exe
bool Install();        // 写入/覆盖（幂等）
bool Uninstall();      // 删除三个入口，并清掉旧 COM 扩展的注册残留
void NotifyShell();    // SHChangeNotify：让资源管理器立刻刷新菜单

// 首次启动的询问只做一次：答过（装或不装）就记下来，之后只由设置里的开关控制
bool PromptAnswered();
void MarkPromptAnswered();

}  // namespace ul::shell
