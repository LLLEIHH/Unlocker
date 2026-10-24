#include "ui/ElevationCheck.h"

#include "core/Privilege.h"

namespace ul {

bool IsUiElevated() { return IsElevated(); }

void PatchDropForElevatedUi(HWND hwnd) {
    if (!hwnd) return;
    // 补丁保证「能用」；横幅保证「用户知道为什么可能不对劲」。只做补丁不留提示，
    // 出问题时没人想得起来是自己提权启动的。
    ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, 0x0049 /*WM_COPYGLOBALDATA*/, MSGFLT_ALLOW, nullptr);
}

}  // namespace ul
