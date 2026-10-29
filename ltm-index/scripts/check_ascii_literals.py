#!/usr/bin/env python3
# scripts/check_ascii_literals.py
#
# 工程约定（docs/status.md「已知开发陷阱」、方案附录 C7）：
#   会进控制台的**字符串字面量**必须是纯 ASCII；**注释与 docs/** 保持中文。
#
# 为什么需要机器化：本机控制台代码页是 GBK(936)。同一个 UTF-8 中文字面量在 C++ 的
# printf 下正常、在 Python 的 print 下变乱码；遇到 GBK 无法表示的字符（如 ∈、—、全角
# 括号）时 Python 直接 UnicodeEncodeError 崩掉 —— 也就是说中文输出在任何一端都不是
# 可靠契约。判据是「这段文本会进控制台吗」。
#
# 这条扫描此前只是收尾时**手工跑过一次**（缺陷 #36 的教训），没有留下脚本。
# 现在留成脚本，可重复执行。
#
# 用法: python scripts/check_ascii_literals.py [--list] [路径...]

import os
import re
import sys

SKIP_DIRS = {"_bak_m0", "_bak_m1", "_bak_m2", "_bak_m3", "_bak_m4", "__pycache__",
             "build-vs", "build-asan", "build-dbg", "build-dbg-soak", "build-fi"}

# 每种扩展名的「注释」与「字面量」形态不同，分开处理
CXX_EXT = {".cc", ".cpp", ".h", ".hpp"}
PY_EXT = {".py"}
SH_EXT = {".sh"}


def strip_cxx(src: str) -> str:
    # 先去掉块注释与行注释。字符串里的 // 会被误伤，但那只会让"该报的没报"变成
    # "少报"，不会产生假阳性 —— 本脚本宁可漏报也不误报，因为误报会让人开始忽略它。
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def strip_hash_comments(src: str) -> str:
    # 对 .py/.sh：先去掉三引号块（文档字符串按"文档"处理，不受本约定约束），
    # 再去掉 # 行注释。
    src = re.sub(r'"""(?:.|\n)*?"""', " ", src)
    src = re.sub(r"'''(?:.|\n)*?'''", " ", src)
    src = re.sub(r"#[^\n]*", " ", src)
    return src


def find_non_ascii_literals(path: str) -> list:
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()
    ext = os.path.splitext(path)[1].lower()
    if ext in CXX_EXT:
        body = strip_cxx(src)
    elif ext in PY_EXT or ext in SH_EXT:
        body = strip_hash_comments(src)
    else:
        return []
    hits = []
    for m in re.finditer(r'"((?:[^"\\\n]|\\.)*)"' r"|'((?:[^'\\\n]|\\.)*)'", body):
        text = m.group(1) if m.group(1) is not None else m.group(2)
        if text and any(ord(c) > 127 for c in text):
            line = body.count("\n", 0, m.start()) + 1
            hits.append((line, text[:70]))
    return hits


def main(argv) -> int:
    show_list = "--list" in argv
    # tools/ 也要扫：c0_boundary_probe 与 soak_mixed 都是往控制台打字的诊断程序，
    # 之前不在默认根里 —— 于是"所有控制台字面量都是 ASCII"这条规矩对它们静默失效，
    # 与缺陷 #37 同型（绿灯来自检查没跑到）。2026-10-01 补上（当时 tools/ 下 2 个文件是干净的）。
    roots = [a for a in argv[1:] if not a.startswith("--")] or [
        "src",
        "tests",
        "demo",
        "scripts",
        "tools",
    ]
    scanned = 0
    bad = 0
    for root in roots:
        if os.path.isfile(root):
            files = [root]
        else:
            files = []
            for dirpath, dirnames, filenames in os.walk(root):
                dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
                files += [os.path.join(dirpath, f) for f in filenames]
        for path in files:
            if os.path.splitext(path)[1].lower() not in (CXX_EXT | PY_EXT | SH_EXT):
                continue
            scanned += 1
            hits = find_non_ascii_literals(path)
            if hits:
                bad += 1
                print("NON-ASCII LITERAL: %s" % path)
                if show_list:
                    for line, text in hits[:10]:
                        print("   line %d: %s" % (line, text))
    print("ascii-literal check: scanned %d files, %d with non-ASCII literals" % (scanned, bad))
    if bad:
        print("FAIL: console-bound string literals must be pure ASCII "
              "(comments and docs/ stay Chinese).")
        return 1
    print("PASS: every console-bound string literal is pure ASCII.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
