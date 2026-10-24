"""把 WinUI 输出目录打成单个载荷文件（供 UnlockerBundle.exe 内嵌）。

格式（全部小端）：

    offset 0   char  magic[8]      "ULKPAK1\\0"
    offset 8   u32   algo          压缩算法（compressapi 的 COMPRESS_ALGORITHM）
    offset 12  u32   count         文件数
    offset 16  u64   tocCompSize   压缩后的 TOC 长度
    offset 24  u64   tocRawSize    TOC 原始长度
    offset 32  blob[tocCompSize]   TOC（压缩）
    ...        blob[]              各文件内容（同样算法压缩，按 TOC 顺序排列）

TOC（解压后）：

    u32 count
    repeat: u16 nameLen, wchar16 name[nameLen], u64 rawSize, u64 compSize

压缩用 Windows 自带的 CompressAPI（cabinet.dll）。选它是因为**生产端和消费端用同一个 API**，
不存在「自己拼压缩流格式」的坑（MSZIP 的块头格式就不是能猜出来的）。本机无 zlib 可链，
实测 XPRESS_HUFF 对 DLL 约 2.3x，62MB 载荷压到 ~27MB，解压 60MB 只要 0.1s。

用法：
    python tools/make_bundle.py --src winui/build-winui/x64/Release --out build/bundle/payload.bin
"""

from __future__ import annotations

import argparse
import ctypes
import os
import struct
import sys
from ctypes import wintypes
from pathlib import Path

COMPRESS_ALGORITHM_XPRESS_HUFF = 4  # compressapi.h

MAGIC = b"ULKPAK1\0"
# 这些是调试/中间产物，分发不需要
SKIP_SUFFIX = {".pdb", ".ilk", ".exp", ".lib", ".xml"}
SKIP_DIRS = {"obj"}


def _compress_api():
    dll = ctypes.WinDLL("cabinet.dll", use_last_error=True)
    dll.CreateCompressor.argtypes = [wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(wintypes.HANDLE)]
    dll.Compress.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                             ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    dll.CloseCompressor.argtypes = [wintypes.HANDLE]
    return dll


def compress(dll, data: bytes, algo: int = COMPRESS_ALGORITHM_XPRESS_HUFF) -> bytes:
    h = wintypes.HANDLE()
    if not dll.CreateCompressor(algo, None, ctypes.byref(h)):
        raise OSError(f"CreateCompressor 失败 err={ctypes.get_last_error()}")
    try:
        out = ctypes.create_string_buffer(len(data) + 1024 * 1024)
        n = ctypes.c_size_t(0)
        if not dll.Compress(h, data, len(data), out, len(out), ctypes.byref(n)):
            raise OSError(f"Compress 失败 err={ctypes.get_last_error()}")
        return out.raw[:n.value]
    finally:
        dll.CloseCompressor(h)


def collect(src: Path) -> list[Path]:
    files: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames[:] = [d for d in dirnames if d.lower() not in SKIP_DIRS]
        for name in filenames:
            p = Path(dirpath) / name
            if p.suffix.lower() in SKIP_SUFFIX:
                continue
            files.append(p)
    files.sort(key=lambda p: p.relative_to(src).as_posix().lower())
    return files


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    src = Path(args.src).resolve()
    out = Path(args.out)
    if not src.is_dir():
        print(f"源目录不存在：{src}", file=sys.stderr)
        return 1

    dll = _compress_api()
    files = collect(src)
    print(f"打包 {len(files)} 个文件，来自 {src}")

    # 先拼 TOC 与数据区
    toc = bytearray(struct.pack("<I", len(files)))
    blobs: list[bytes] = []
    raw_total = 0
    comp_total = 0
    for p in files:
        data = p.read_bytes()
        rel = p.relative_to(src).as_posix()
        blob = compress(dll, data)
        name = rel.encode("utf-16-le")
        toc += struct.pack("<H", len(name) // 2) + name + struct.pack("<QQ", len(data), len(blob))
        blobs.append(blob)
        raw_total += len(data)
        comp_total += len(blob)

    toc_comp = compress(dll, bytes(toc))

    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<IIQQ", COMPRESS_ALGORITHM_XPRESS_HUFF, len(files),
                            len(toc_comp), len(toc)))
        f.write(toc_comp)
        for blob in blobs:
            f.write(blob)

    size = out.stat().st_size
    print(f"原始 {raw_total / 1048576:.1f} MB → 载荷 {size / 1048576:.1f} MB"
          f"（{raw_total / max(1, size):.2f}x）")
    print(f"写出 {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
