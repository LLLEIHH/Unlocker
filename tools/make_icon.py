"""Unlocker 图标生成器 —— 单一几何源，同时产出 SVG / PNG / ICO。

本机没有 ImageMagick / Inkscape / Pillow / numpy，所以这里用纯 Python 做：
  · 几何用有向距离函数（SDF）描述，逐像素算覆盖率（自带抗锯齿）
  · 先在 SS 倍超采样下渲染，再盒式降采样到目标尺寸
  · ICO 用 BMP(32bpp) 装小尺寸、PNG 装 128/256（Vista+ 支持 PNG 条目）

用法：
    python tools/make_icon.py --variant a1 --png out.png --size 256
    python tools/make_icon.py --variant a1 --svg assets/icons/unlock-a1.svg
    python tools/make_icon.py --variant a1 --ico assets/icons/unlocker.ico

设计空间固定 256×256：圆角方块 8..248（r=56），锁体 72..184 × 112..200（r=18）。
所有变体共用同一套容器与锁体，只在锁梁上做文章，保证风格一致。
"""

from __future__ import annotations

import argparse
import math
import struct
import zlib
from pathlib import Path

DESIGN = 256.0

# ---------------------------------------------------------------- 几何常量
TILE = (8.0, 8.0, 248.0, 248.0, 56.0)     # x0,y0,x1,y1,r
BODY = (72.0, 124.0, 184.0, 216.0, 18.0)
ARC = (128.0, 75.0, 26.0, 18.0)           # cx, cy, 半径, 线宽 → 弧外沿 = 40
LEG_R = 9.0
KEY_C = (128.0, 158.0, 12.0)              # 钥匙孔圆
KEY_SLOT = [(121.0, 166.0), (135.0, 166.0), (140.0, 196.0), (116.0, 196.0)]

BG_TOP = (0x3B, 0x82, 0xF6)
BG_BOTTOM = (0x1D, 0x4E, 0xD8)
FG = (255, 255, 255)

# 变体：锁梁由「弧段角度区间 + 两条腿」组成
#   角度以「右=0°、上=90°、左=180°」计；两腿 x = 102 / 154
VARIANTS = {
    "a1": {
        "title": "微开：左腿插进锁体，右腿短一截悬在上方",
        "arcs": [(0.0, 180.0)],
        "legs": [((102.0, 130.0), (102.0, 75.0)), ((154.0, 75.0), (154.0, 106.0))],
    },
    "a2": {
        "title": "全开：锁梁整体抬起，与锁体脱开",
        "arcs": [(0.0, 180.0)],
        "legs": [((102.0, 75.0), (102.0, 96.0)), ((154.0, 75.0), (154.0, 96.0))],
    },
    "a3": {
        "title": "断开：两腿都接锁体，顶端断开一截",
        "arcs": [(0.0, 55.0), (125.0, 180.0)],
        "legs": [((102.0, 130.0), (102.0, 75.0)), ((154.0, 130.0), (154.0, 75.0))],
    },
    # 原 A 版：锁梁左移 14 单位，弧心与锁体主轴都落在 x=128；整体下移 6 单位让上下留白相等
    "a4": {
        "title": "原版 A（锁梁居中）：左腿插入锁体，右腿悬空",
        "arc": (128.0, 92.0, 34.0, 18.0),
        "arcs": [(0.0, 180.0)],
        "legs": [((94.0, 118.0), (94.0, 92.0)), ((162.0, 92.0), (162.0, 102.0))],
        "body": (74.0, 114.0, 182.0, 206.0, 16.0),
        "key_c": (128.0, 150.0, 11.0),
        "key_slot": [(123.0, 158.0), (133.0, 158.0), (138.0, 180.0), (118.0, 180.0)],
    },
}


def spec(v: dict) -> dict:
    """补齐默认值，让变体只写需要覆盖的项。"""
    out = {
        "arc": ARC,
        "arcs": [],
        "legs": [],
        "body": BODY,
        "key_c": KEY_C,
        "key_slot": KEY_SLOT,
    }
    out.update({k: val for k, val in v.items() if k != "title"})
    out["title"] = v["title"]
    return out


# ---------------------------------------------------------------- SDF
def sdf_round_rect(px, py, x0, y0, x1, y1, r):
    cx, cy = (x0 + x1) * 0.5, (y0 + y1) * 0.5
    hw, hh = (x1 - x0) * 0.5 - r, (y1 - y0) * 0.5 - r
    qx, qy = abs(px - cx) - hw, abs(py - cy) - hh
    ax, ay = max(qx, 0.0), max(qy, 0.0)
    return math.hypot(ax, ay) + min(max(qx, qy), 0.0) - r


def sdf_circle(px, py, cx, cy, r):
    return math.hypot(px - cx, py - cy) - r


def sdf_segment(px, py, a, b, r):
    ax, ay = a
    bx, by = b
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    denom = vx * vx + vy * vy
    t = 0.0 if denom == 0 else max(0.0, min(1.0, (wx * vx + wy * vy) / denom))
    return math.hypot(wx - t * vx, wy - t * vy) - r


def sdf_polygon(px, py, pts):
    """凸多边形：取所有边的半平面最大值（本文用到的槽口都是凸的）。"""
    n = len(pts)
    inside = True
    best = -1e9
    # 统一绕向（顺/逆都支持）
    area = 0.0
    for i in range(n):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % n]
        area += x0 * y1 - x1 * y0
    sign = 1.0 if area > 0 else -1.0
    for i in range(n):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % n]
        ex, ey = x1 - x0, y1 - y0
        ln = math.hypot(ex, ey)
        # 指向外侧的法线
        nx, ny = sign * ey / ln, -sign * ex / ln
        d = (px - x0) * nx + (py - y0) * ny
        best = max(best, d)
        if d > 0:
            inside = False
    return best if inside else -best


def sdf_arc(px, py, cx, cy, r, w, a0, a1):
    """圆弧带（含两端圆头）。a0/a1 为角度区间，单位度，右=0 上=90 左=180。"""
    dx, dy = px - cx, -(py - cy)
    ang = math.degrees(math.atan2(dy, dx))
    if ang < 0:
        ang += 360.0
    half = w * 0.5
    d_ring = abs(math.hypot(dx, dy) - r) - half
    lo, hi = math.radians(a0), math.radians(a1)
    # 端点距离（圆头）
    p0 = (cx + r * math.cos(lo), cy - r * math.sin(lo))
    p1 = (cx + r * math.cos(hi), cy - r * math.sin(hi))
    d_caps = min(math.hypot(px - p0[0], py - p0[1]), math.hypot(px - p1[0], py - p1[1])) - half
    a0n, a1n = ang, ang
    if a0 <= a1:
        in_range = a0 <= a0n <= a1
    else:
        in_range = a0n >= a0 or a0n <= a1
    return d_ring if in_range else d_caps


# ---------------------------------------------------------------- 渲染
def gradient_at(y):
    t = max(0.0, min(1.0, (y - TILE[1]) / (TILE[3] - TILE[1])))
    return tuple(int(round(a + (b - a) * t)) for a, b in zip(BG_TOP, BG_BOTTOM))


def render(variant: str, size: int, ss: int = 4):
    """返回 (w,h,bytearray RGBA)，尺寸为 size×size。"""
    v = spec(VARIANTS[variant])
    s = size * ss / DESIGN          # 设计单位 → 设备像素
    W = size * ss

    buf = bytearray(W * W * 4)      # RGBA，非预乘

    def blend(idx, color, cov):
        if cov <= 0:
            return
        r, g, b = color
        i = idx
        a = cov
        dr, dg, db, da = buf[i], buf[i + 1], buf[i + 2], buf[i + 3]
        inv = 1.0 - a
        na = a + (da / 255.0) * inv
        if na <= 0:
            return
        buf[i] = int(round((r * a + dr * (da / 255.0) * inv) / na))
        buf[i + 1] = int(round((g * a + dg * (da / 255.0) * inv) / na))
        buf[i + 2] = int(round((b * a + db * (da / 255.0) * inv) / na))
        buf[i + 3] = int(round(na * 255))

    def fill(sdf, color_fn, bbox):
        """只遍历形状外接框（设计单位），把 W² 的全画布扫描降到一个零头。"""
        x0 = max(0, int((bbox[0] - 1) * s))
        y0 = max(0, int((bbox[1] - 1) * s))
        x1 = min(W, int((bbox[2] + 2) * s))
        y1 = min(W, int((bbox[3] + 2) * s))
        for y in range(y0, y1):
            row = y * W * 4
            dyy = (y + 0.5) / s
            for x in range(x0, x1):
                d = sdf((x + 0.5) / s, dyy) * s
                cov = 0.5 - d
                if cov <= 0.0:
                    continue
                if cov > 1.0:
                    cov = 1.0
                blend(row + x * 4, color_fn(dyy), cov)

    fill(lambda x, y: sdf_round_rect(x, y, *TILE), gradient_at, (TILE[0], TILE[1], TILE[2], TILE[3]))
    cx0, cy0, r0, w0 = v["arc"]
    for a0, a1 in v["arcs"]:
        fill(lambda x, y, a0=a0, a1=a1: sdf_arc(x, y, cx0, cy0, r0, w0, a0, a1), lambda yy: FG,
             (cx0 - r0 - w0, cy0 - r0 - w0, cx0 + r0 + w0, cy0 + w0))
    for a, b in v["legs"]:
        fill(lambda x, y, a=a, b=b: sdf_segment(x, y, a, b, LEG_R), lambda yy: FG,
             (min(a[0], b[0]) - LEG_R, min(a[1], b[1]) - LEG_R,
              max(a[0], b[0]) + LEG_R, max(a[1], b[1]) + LEG_R))
    body = v["body"]
    fill(lambda x, y: sdf_round_rect(x, y, *body), lambda yy: FG,
         (body[0], body[1], body[2], body[3]))
    kc = v["key_c"]
    fill(lambda x, y: sdf_circle(x, y, *kc), gradient_at,
         (kc[0] - kc[2], kc[1] - kc[2], kc[0] + kc[2], kc[1] + kc[2]))
    slot_pts = v["key_slot"]
    xs = [p[0] for p in slot_pts]
    ys = [p[1] for p in slot_pts]
    fill(lambda x, y: sdf_polygon(x, y, slot_pts), gradient_at,
         (min(xs), min(ys), max(xs), max(ys)))

    # 盒式降采样
    out = bytearray(size * size * 4)
    n = ss * ss
    for oy in range(size):
        for ox in range(size):
            r = g = b = a = 0
            for dy in range(ss):
                base = ((oy * ss + dy) * W + ox * ss) * 4
                for dx in range(ss):
                    i = base + dx * 4
                    r += buf[i]
                    g += buf[i + 1]
                    b += buf[i + 2]
                    a += buf[i + 3]
            o = (oy * size + ox) * 4
            out[o] = r // n
            out[o + 1] = g // n
            out[o + 2] = b // n
            out[o + 3] = a // n
    return size, size, out


# ---------------------------------------------------------------- 输出
def write_png(path: Path, w: int, h: int, rgba: bytes) -> None:
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
                     chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def bmp_entry(size: int, rgba: bytes) -> bytes:
    """32bpp 图标条目：BITMAPINFOHEADER + 自下而上的 BGRA + 全零 AND 掩码。"""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = bytearray()
    stride = size * 4
    for y in range(size - 1, -1, -1):
        row = rgba[y * stride:(y + 1) * stride]
        for x in range(size):
            i = x * 4
            pixels += bytes((row[i + 2], row[i + 1], row[i], row[i + 3]))
    mask_stride = ((size + 31) // 32) * 4
    mask = bytes(mask_stride * size)
    return header + bytes(pixels) + mask


def write_ico(path: Path, variant: str, sizes=(256, 128, 64, 48, 32, 24, 16)) -> None:
    images = []
    for s in sizes:
        w, h, rgba = render(variant, s, ss=4 if s <= 64 else 2)
        if s >= 128:
            tmp = path.with_suffix(f".tmp{s}.png")
            write_png(tmp, w, h, rgba)
            blob = tmp.read_bytes()
            tmp.unlink()
        else:
            blob = bmp_entry(s, rgba)
        images.append((s, blob))

    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for s, blob in images:
        out += struct.pack("<BBBBHHII", 0 if s >= 256 else s, 0 if s >= 256 else s,
                           0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    for _, blob in images:
        out += blob
    path.write_bytes(bytes(out))


def write_svg(path: Path, variant: str) -> None:
    v = spec(VARIANTS[variant])
    cx, cy, r, w = v["arc"]
    top, bottom = "#%02X%02X%02X" % BG_TOP, "#%02X%02X%02X" % BG_BOTTOM
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256" '
        f'role="img" aria-label="Unlocker icon {variant}">',
        f"  <!-- {v['title']} -->",
        "  <defs>",
        '    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">',
        f'      <stop offset="0" stop-color="{top}"/>',
        f'      <stop offset="1" stop-color="{bottom}"/>',
        "    </linearGradient>",
        "  </defs>",
        f'  <rect x="8" y="8" width="240" height="240" rx="56" fill="url(#bg)"/>',
    ]

    def pt(angle_deg):
        a = math.radians(angle_deg)
        return cx + r * math.cos(a), cy - r * math.sin(a)

    for a0, a1 in v["arcs"]:
        x0, y0 = pt(a0)
        x1, y1 = pt(a1)
        large = 1 if (a1 - a0) > 180 else 0
        # SVG 弧：sweep=1 表示顺时针（角度递减方向），这里角度递增 → sweep=0
        parts.append(
            f'  <path d="M{x0:.2f} {y0:.2f} A{r} {r} 0 {large} 0 {x1:.2f} {y1:.2f}" '
            f'fill="none" stroke="#FFFFFF" stroke-width="{w}" stroke-linecap="round"/>')
    for (ax, ay), (bx, by) in v["legs"]:
        parts.append(f'  <line x1="{ax}" y1="{ay}" x2="{bx}" y2="{by}" stroke="#FFFFFF" '
                     f'stroke-width="{LEG_R * 2}" stroke-linecap="round"/>')
    x0, y0, x1, y1, rr = v["body"]
    parts.append(f'  <rect x="{x0}" y="{y0}" width="{x1 - x0}" height="{y1 - y0}" rx="{rr}" fill="#FFFFFF"/>')
    kx, ky, kr = v["key_c"]
    parts.append(f'  <circle cx="{kx}" cy="{ky}" r="{kr}" fill="url(#bg)"/>')
    slot = " ".join(f"{'M' if i == 0 else 'L'}{x:.0f} {y:.0f}" for i, (x, y) in enumerate(v["key_slot"]))
    parts.append(f'  <path d="{slot} Z" fill="url(#bg)"/>')
    parts.append("</svg>")
    path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def make_sheet(path: Path, variants, big: int = 180) -> None:
    """三方案对照图：大图一行 + 48/32/24/16 实际尺寸一行。"""
    small = (48, 32, 24, 16)
    gap = 24
    margin = 16
    width = margin * 2 + len(variants) * big + (len(variants) - 1) * gap
    height = margin * 2 + big + 18 + small[0]
    W, H = width, height
    canvas = bytearray(b"\xF3\xF4\xF6\xFF" * (W * H))

    def paste(size: int, rgba: bytes, ox: int, oy: int):
        for y in range(size):
            for x in range(size):
                i = (y * size + x) * 4
                a = rgba[i + 3]
                if a == 0:
                    continue
                j = ((oy + y) * W + ox + x) * 4
                inv = 255 - a
                for k in range(3):
                    canvas[j + k] = (rgba[i + k] * a + canvas[j + k] * inv) // 255
                canvas[j + 3] = 255

    for idx, v in enumerate(variants):
        ox = margin + idx * (big + gap)
        w, h, rgba = render(v, big, ss=3)
        paste(big, rgba, ox, margin)
        sx = ox
        sy = margin + big + 18
        for s in small:
            ws, hs, rs = render(v, s, ss=4)
            paste(s, rs, sx, sy + small[0] - s)
            sx += s + 10

    out = bytearray(W * H * 4)
    out[:] = canvas
    write_png(path, W, H, out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", choices=sorted(VARIANTS))
    ap.add_argument("--png")
    ap.add_argument("--size", type=int, default=256)
    ap.add_argument("--svg")
    ap.add_argument("--ico")
    ap.add_argument("--sheet", help="输出多方案对照图（大图 + 实际小尺寸）")
    ap.add_argument("--variants", default="a1,a2,a3", help="对照图包含的变体，逗号分隔")
    args = ap.parse_args()

    if args.sheet:
        make_sheet(Path(args.sheet), args.variants.split(","))
        print(f"png  {args.sheet}")
        return 0
    if not args.variant:
        ap.error("--png/--svg/--ico 需要 --variant")

    if args.svg:
        Path(args.svg).parent.mkdir(parents=True, exist_ok=True)
        write_svg(Path(args.svg), args.variant)
        print(f"svg  {args.svg}")
    if args.png:
        w, h, rgba = render(args.variant, args.size, ss=4)
        write_png(Path(args.png), w, h, rgba)
        print(f"png  {args.png} ({w}x{h})")
    if args.ico:
        Path(args.ico).parent.mkdir(parents=True, exist_ok=True)
        write_ico(Path(args.ico), args.variant)
        print(f"ico  {args.ico} ({Path(args.ico).stat().st_size // 1024} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
