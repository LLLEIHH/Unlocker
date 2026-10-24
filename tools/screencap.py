"""最小屏幕截图工具（不依赖 Pillow / 不加载 .NET）。

沙箱禁了 PowerShell 的 Add-Type，Pillow 也没装，所以直接走 GDI32：
GetDC(NULL) → CreateCompatibleBitmap → BitBlt → GetDIBits → 手写 PNG（zlib + CRC32）。

用法：python tools/screencap.py out.png [x y w h]
只用于开发期人工核对界面布局。
"""

from __future__ import annotations

import ctypes
import struct
import sys
import time
import zlib
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

SRCCOPY = 0x00CC0020
DIB_RGB_COLORS = 0

# 关键：本进程必须声明 DPI 感知，否则系统会把 GetWindowRect / BitBlt 的坐标全部按
# 「虚拟化缩放」折算（150% 下真实 1230x840 的窗口会被读成 820x560），截出来既错位又糊。
try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # PER_MONITOR_AWARE_V2
except Exception:
    try:
        user32.SetProcessDPIAware()
    except Exception:
        pass


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


def _bind():
    """显式声明原型：64 位下句柄按 int 传递会被截断（GetDIBits 报 1400）。"""
    user32.GetDC.argtypes = [wintypes.HWND]
    user32.GetDC.restype = wintypes.HDC
    user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
    user32.GetSystemMetrics.argtypes = [ctypes.c_int]
    user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.FindWindowW.restype = wintypes.HWND
    user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                    ctypes.c_int, ctypes.c_int, ctypes.c_uint]
    user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]

    gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
    gdi32.CreateCompatibleDC.restype = wintypes.HDC
    gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
    gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
    gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
    gdi32.SelectObject.restype = wintypes.HGDIOBJ
    gdi32.BitBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                             wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD]
    gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
                                ctypes.c_void_p, ctypes.POINTER(BITMAPINFOHEADER), wintypes.UINT]
    gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
    gdi32.DeleteDC.argtypes = [wintypes.HDC]


def capture(x: int, y: int, w: int, h: int) -> bytes:
    """返回 RGB 像素（每行 w*3 字节，自上而下）。"""
    stride = ((w * 3 + 3) // 4) * 4  # DIB 行按 4 字节对齐
    screen = user32.GetDC(None)
    memdc = gdi32.CreateCompatibleDC(screen)
    bmp = gdi32.CreateCompatibleBitmap(screen, w, h)
    old = gdi32.SelectObject(memdc, bmp)
    try:
        if not gdi32.BitBlt(memdc, 0, 0, w, h, screen, x, y, SRCCOPY):
            raise OSError(f"BitBlt failed: {ctypes.get_last_error()}")
        # GetDIBits 要求位图**不**被任何 DC 选中，所以先还原
        gdi32.SelectObject(memdc, old)
        bi = BITMAPINFOHEADER()
        bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bi.biWidth = w
        bi.biHeight = -h  # 负值 = 自上而下
        bi.biPlanes = 1
        bi.biBitCount = 24
        bi.biCompression = 0
        buf = ctypes.create_string_buffer(stride * h)
        got = gdi32.GetDIBits(screen, bmp, 0, h, buf, ctypes.byref(bi), DIB_RGB_COLORS)
        if got == 0:
            raise OSError(f"GetDIBits failed: {ctypes.get_last_error()}")
        raw = buf.raw
    finally:
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(memdc)
        user32.ReleaseDC(None, screen)

    # DIB 每行按 4 字节对齐，宽度不是 4 的倍数时要裁掉行尾补齐
    if stride != w * 3:
        packed = bytearray(w * 3 * h)
        for row in range(h):
            packed[row * w * 3:(row + 1) * w * 3] = raw[row * stride:row * stride + w * 3]
        raw = bytes(packed)

    # BGR → RGB
    out = bytearray(len(raw))
    out[0::3] = raw[2::3]
    out[1::3] = raw[1::3]
    out[2::3] = raw[0::3]
    return bytes(out)


def write_png(path: str, w: int, h: int, rgb: bytes) -> None:
    stride = w * 3
    raw = bytearray()
    for row in range(h):
        raw.append(0)  # filter type 0
        raw += rgb[row * stride:(row + 1) * stride]

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8bit truecolor
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def find_window_for_pid(pid: int) -> int:
    """该进程里面积最大的可见顶层窗口（WinUI 主窗口）。"""
    best = [0, 0]
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(hwnd, _lp):
        if not user32.IsWindowVisible(hwnd):
            return True
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid:
            return True
        rc = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rc))
        area = (rc.right - rc.left) * (rc.bottom - rc.top)
        if area > best[1]:
            best[0], best[1] = hwnd, area
        return True

    user32.EnumWindows(proc(cb), 0)
    return best[0]


def main() -> int:
    _bind()
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]

    if len(sys.argv) >= 4 and sys.argv[2] == "--pid":
        pid = int(sys.argv[3])
        hwnd = find_window_for_pid(pid)
        if not hwnd:
            print(f"no window for pid {pid}", file=sys.stderr)
            return 1
    elif len(sys.argv) >= 4 and sys.argv[2] == "--window":
        title = sys.argv[3]
        hwnd = user32.FindWindowW(None, title)
        if not hwnd:
            print(f"window not found: {title}", file=sys.stderr)
            return 1
    else:
        hwnd = 0

    if hwnd:
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
        # 后台进程没有前台权限，SetForegroundWindow 常常被系统忽略；
        # 用 TOPMOST 强占 z 序（截完再还原），这样 BitBlt 才能拍到本窗口。
        HWND_TOPMOST = -1
        HWND_NOTOPMOST = -2
        SWP = 0x0001 | 0x0002 | 0x0040  # NOSIZE | NOMOVE | SHOWWINDOW
        user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP)
        time.sleep(0.5)
        rc = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rc))
        x, y, w, h = rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top
        data = capture(x, y, w, h)
        user32.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP)
        write_png(path, w, h, data)
        print(f"saved {path} ({w}x{h} at {x},{y})")
        return 0

    if len(sys.argv) >= 6:
        x, y, w, h = (int(v) for v in sys.argv[2:6])
    else:
        x, y = 0, 0
        w = user32.GetSystemMetrics(0)
        h = user32.GetSystemMetrics(1)

    write_png(path, w, h, capture(x, y, w, h))
    print(f"saved {path} ({w}x{h} at {x},{y})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
