#pragma once
// 启动时自查是否被用户手动提权（B7 兜底）。
// manifest 不能阻止用户右键「以管理员身份运行」，而一旦 UI 变成高完整性，
// 拖放和右键菜单会静默失效 —— 补丁 + 横幅两条都要做。
#include <windows.h>

namespace ul {

bool IsUiElevated();

// 只在 IsUiElevated() 为真时调用；此时是我们自己的窗口，风险可控
void PatchDropForElevatedUi(HWND hwnd);

}  // namespace ul
