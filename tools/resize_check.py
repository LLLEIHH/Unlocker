"""开发期辅助：把 Unlocker 窗口缩到指定尺寸，验证最小尺寸是否生效。

用法：python tools/resize_check.py <pid> <w> <h>
输出窗口实际尺寸（物理像素）；期望被 PreferredMinimum{Width,Height} 挡住，不会小于 1230x840（150% DPI 下）。
"""

from __future__ import annotations

import ctypes
import sys
import time
from ctypes import wintypes

u = ctypes.WinDLL("user32", use_last_error=True)
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

EP = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def main_window(pid: int) -> int:
    best = [0, 0]

    def cb(hwnd, _lp):
        if not u.IsWindowVisible(hwnd):
            return True
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value != pid:
            return True
        rc = wintypes.RECT()
        u.GetWindowRect(hwnd, ctypes.byref(rc))
        area = (rc.right - rc.left) * (rc.bottom - rc.top)
        if area > best[1]:
            best[0], best[1] = hwnd, area
        return True

    u.EnumWindows(EP(cb), 0)
    return best[0]


def size(hwnd: int) -> tuple[int, int]:
    rc = wintypes.RECT()
    u.GetWindowRect(ctypes.c_void_p(hwnd), ctypes.byref(rc))
    return rc.right - rc.left, rc.bottom - rc.top


def main() -> int:
    pid, w, h = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    hwnd = main_window(pid)
    if not hwnd:
        print("no window", file=sys.stderr)
        return 1
    print("before", *size(hwnd))
    u.ShowWindow(ctypes.c_void_p(hwnd), 9)  # SW_RESTORE（后台启动的实例常常是 minimized）
    time.sleep(0.3)
    SWP_NOMOVE, SWP_NOZORDER = 0x0002, 0x0004
    u.SetWindowPos(ctypes.c_void_p(hwnd), None, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER)
    time.sleep(0.4)
    print("after ", *size(hwnd))
    return 0


if __name__ == "__main__":
    sys.exit(main())
