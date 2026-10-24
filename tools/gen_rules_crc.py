#!/usr/bin/env python3
"""从 Blacklist.def 生成规则表 CRC（fail-closed 自检用）。

运行时 PolicyGuard::SerializeRulesForCrc() 逐行输出：
    id|type|pattern|verdict|severity|reason\n
其中 id 是 RuleId 枚举值（BL_NONE=0，其余按 .def 中 BL_RULE_ID 出现顺序），
type/verdict/severity 是枚举整数值，pattern/reason 是 UTF-8 原文。

本脚本必须与上面的序列化逐字节一致，否则启动时 CRC 校验会误判。
"""
import re
import sys
import zlib

# Windows 上 sys.stdout 默认走 ANSI 代码页（CI 英文镜像是 cp1252），打印中文会直接
# UnicodeEncodeError 崩掉 —— 校验结果本身没错，却把构建带挂了（2026-10-24 发布时踩到）。
# 统一强制 UTF-8 并对无法编码的字符降级替换，让脚本在任何控制台上都能跑完。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


TYPE = {
    "MATCH_EXACT": 0,
    "MATCH_PREFIX": 1,
    "MATCH_GLOB": 2,
    "MATCH_FILE_ID": 3,
    "MATCH_VOLUME_ROOT": 4,
    "MATCH_VOLUME": 5,
    "MATCH_SELF": 6,
    "MATCH_COUNT": 7,
}
VERDICT = {"ALLOW": 0, "CONFIRM": 1, "DENY": 2}
SEVERITY = {"INFO": 0, "HIGH": 1, "CRIT": 2}

ID_RE = re.compile(r"^\s*BL_RULE_ID\(\s*(\w+)\s*\)")
RULE_RE = re.compile(
    r'^\s*BL_RULE\(\s*(\w+)\s*,\s*(\w+)\s*,\s*LR"\((.*?)\)"\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*"(.*?)"\s*\)'
)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: gen_rules_crc.py <Blacklist.def> <out.h>", file=sys.stderr)
        return 2

    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "r", encoding="utf-8") as f:
        text = f.read()

    ids = {}
    for line in text.splitlines():
        m = ID_RE.match(line)
        if m and m.group(1) not in ids:
            ids[m.group(1)] = len(ids) + 1  # BL_NONE = 0

    payload = []
    count = 0
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.strip().startswith("BL_RULE("):
            continue
        m = RULE_RE.match(line)
        if not m:
            print(f"cannot parse line {lineno}: {line}", file=sys.stderr)
            return 1
        rid, mtype, pattern, verdict, severity, reason = m.groups()
        if rid not in ids:
            print(f"unknown rule id {rid} at line {lineno}", file=sys.stderr)
            return 1
        payload.append(
            "%d|%d|%s|%d|%d|%s\n"
            % (
                ids[rid],
                TYPE[mtype],
                pattern,
                VERDICT[verdict],
                SEVERITY[severity],
                reason,
            )
        )
        count += 1

    data = "".join(payload).encode("utf-8")
    crc = zlib.crc32(data) & 0xFFFFFFFF

    with open(dst, "w", encoding="utf-8") as f:
        f.write("// 自动生成，请勿手改；由 tools/gen_rules_crc.py 从 Blacklist.def 计算\n")
        f.write("#pragma once\n")
        f.write(f"#define UNLOCKER_RULES_CRC 0x{crc:08X}u\n")
        f.write("#define UNLOCKER_RULES_CRC_VALID 1\n")
        f.write(f"#define UNLOCKER_RULES_CRC_ROWS {count}\n")

    print(f"rules={count} crc=0x{crc:08X} -> {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
