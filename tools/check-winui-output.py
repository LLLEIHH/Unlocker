#!/usr/bin/env python3
"""WinUI 构建产物校验（构建后自动执行）。

背景：M5.1 时代踩过的坑全靠「人工看一眼输出目录」，一次重建忘记复制 PRI 就会
得到「双击无反应 / 闪退」且没有任何提示。这个脚本把当时的肉眼检查固化下来，
让 build.cmd winui 在产物不全时直接失败。

用法：
    python tools/check-winui-output.py [输出目录]
默认输出目录：winui/build-winui/x64/Release
退出码：0 全部通过；1 有缺失/内容不符。
"""

from __future__ import annotations

import sys
from pathlib import Path

# Windows 上 sys.stdout 默认走 ANSI 代码页（CI 英文镜像是 cp1252），打印中文会直接
# UnicodeEncodeError 崩掉 —— 校验结果本身没错，却把构建带挂了（2026-10-24 发布时踩到）。
# 统一强制 UTF-8 并对无法编码的字符降级替换，让脚本在任何控制台上都能跑完。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


# (文件名, 最小字节数)：小于该值即视为异常（空文件 / 截断）
REQUIRED_FILES = [
    ("Unlocker.exe", 200_000),
    ("Unlocker.pri", 100_000),
    ("resources.pri", 100_000),          # MRT unpackaged 约定的应用资源库名
    ("App.xbf", 200),                    # App.xaml 编译产物
    ("MainWindow.xbf", 2_000),           # MainWindow.xaml 编译产物
    ("Microsoft.UI.Xaml.dll", 500_000),  # WinUI 框架本体（自包含部署）
    ("Microsoft.WindowsAppRuntime.dll", 500_000),
    ("Microsoft.UI.Xaml.Controls.pri", 500_000),  # 框架控件样式资源
]

# PRI 里必须能按 **ASCII** 找到的资源名。
# 注意：PRI 的 map 是「子树 + 名称」的层次结构，资源名不存完整 URI，所以这里只查基名
# （编码是 UTF-8/ASCII，不是 UTF-16 —— 别按宽字符去搜，会永远搜不到）。
PRI_MUST_CONTAIN = [
    "themeresources.xbf",   # 框架控件默认样式（XamlControlsResources 的载荷）
    "App.xbf",
    "MainWindow.xbf",
]


def check(out_dir: Path) -> list[str]:
    errors: list[str] = []

    if not out_dir.is_dir():
        return [f"输出目录不存在：{out_dir}"]

    for name, min_size in REQUIRED_FILES:
        p = out_dir / name
        if not p.is_file():
            errors.append(f"缺少文件：{name}")
            continue
        size = p.stat().st_size
        if size < min_size:
            errors.append(f"文件过小：{name} = {size} 字节（应 ≥ {min_size}）")

    pri = out_dir / "Unlocker.pri"
    if pri.is_file():
        blob = pri.read_bytes()
        for res in PRI_MUST_CONTAIN:
            if res.encode("ascii") not in blob:
                errors.append(f"Unlocker.pri 中找不到资源：{res}")

    main_pri = out_dir / "Unlocker.pri"
    alias_pri = out_dir / "resources.pri"
    if main_pri.is_file() and alias_pri.is_file():
        if main_pri.stat().st_size != alias_pri.stat().st_size:
            errors.append(
                "resources.pri 与 Unlocker.pri 大小不一致（副本过期，检查 "
                "UnlockerApp.vcxproj 的 UnlockerResourcesPri 目标）"
            )

    return errors


def main() -> int:
    if len(sys.argv) > 1:
        out_dir = Path(sys.argv[1])
    else:
        out_dir = Path(__file__).resolve().parent.parent / "winui" / "build-winui" / "x64" / "Release"

    errors = check(out_dir)
    if errors:
        print(f"[FAIL] WinUI 产物校验未通过（{out_dir}）：", file=sys.stderr)
        for e in errors:
            print(f"       - {e}", file=sys.stderr)
        return 1

    print(f"[ OK ] WinUI 产物校验通过（{out_dir}，{len(REQUIRED_FILES)} 项文件 + PRI 资源名）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
