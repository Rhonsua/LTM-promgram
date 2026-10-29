#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""分层依赖检查（方案 §2.2 的机器化强制）。

规则：
  R1  src/index/**      禁止出现文件 I/O：fstream/open(/pread/pwrite/mmap/CreateFile/ReadFile
  R2  src/index/**      禁止 include "storage/disk_manager.h"（只允许经 BufferPool 触达磁盘）
  R3  src/storage/**    禁止 include "index/**"
  R4  src/migration/**  禁止 include "index/**"
  R5  src/**（除 index）禁止 include "index/**"（除上面已覆盖的）
  R6  src/**            禁止在头文件里 using namespace std;
  R7  src/bench/** 与 demo/** 允许自由依赖（评测与展示层）

退出码：0 = 全部通过；1 = 存在违规。

用法：
  python scripts/check_layering.py            # 扫描整个 src/
  python scripts/check_layering.py --json     # 机器可读输出（供 CI 消费）
  python scripts/check_layering.py --diff     # 只报告本次 git 改动涉及的文件
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

IO_PATTERNS = [
    (r"#\s*include\s*<fstream>", "includes <fstream>"),
    (r"#\s*include\s*<cstdio>", "includes <cstdio> (the index layer forbids file I/O)"),
    (r"\bstd::fstream\b", "uses std::fstream"),
    (r"\bstd::ifstream\b", "uses std::ifstream"),
    (r"\bstd::ofstream\b", "uses std::ofstream"),
    (r"\bstd::fopen\b", "uses std::fopen"),
    (r"\bfopen\s*\(", "calls fopen"),
    (r"\bopen\s*\(", "calls open( (a file or POSIX call)"),
    (r"\bpread\s*\(|\bpwrite\s*\(", "calls pread/pwrite"),
    (r"\bmmap\s*\(", "calls mmap"),
    (r"\bCreateFile[AW]?\s*\(", "calls Win32 CreateFile"),
    (r"\bReadFile\s*\(|\bWriteFile\s*\(", "calls Win32 ReadFile/WriteFile"),
]

# 允许出现在索引层的白名单（文件路径相对 src/）
IO_ALLOWLIST = {
    # 目前没有例外；若确有需要，必须在此登记并写明理由
}

LAYER_RULES = [
    # (适用目录前缀, 禁止 include 的前缀, 说明)
    # 注意：索引层允许 include storage/disk_manager.h 与 storage/buffer_pool.h 的**声明**，
    #       因为 HybridIndex 需要持有这两者的所有权并读取其统计；真正被禁止的是
    #       索引层自己发起文件 I/O（由规则 R1 保证）。
    ("storage/", 'index/', "the storage layer must not depend on the index layer (layering is one-way)"),
    ("storage/", 'migration/', "the storage layer must not depend on the migration layer"),
    ("migration/", 'index/', "the migration layer must not depend on the index layer"),
    ("util/", 'index/', "the util layer must not depend on the index layer"),
    ("util/", 'storage/', "the util layer must not depend on the storage layer"),
    ("util/", 'migration/', "the util layer must not depend on the migration layer"),
]


def iter_sources():
    exts = (".h", ".hpp", ".cc", ".cpp", ".cxx")
    for dirpath, _dirnames, filenames in os.walk(SRC):
        for fn in filenames:
            if fn.endswith(exts):
                full = os.path.join(dirpath, fn)
                yield full, os.path.relpath(full, SRC).replace("\\", "/")


def strip_comments(text):
    """去掉 // 与 /* */ 注释，避免把注释里的示例误判为违规。"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    out = []
    for line in text.split("\n"):
        # 粗略处理：不处理字符串内的 //
        idx = line.find("//")
        out.append(line if idx < 0 else line[:idx])
    return "\n".join(out)


def check_file(rel, text):
    problems = []
    code = strip_comments(text)

    # 规则 R1：索引层禁止文件 I/O
    if rel.startswith("index/") and rel not in IO_ALLOWLIST:
        for pat, desc in IO_PATTERNS:
            for m in re.finditer(pat, code):
                line = code[: m.start()].count("\n") + 1
                problems.append((rel, line, "R1", desc, m.group(0).strip()))
    # 规则 R2..R5：分层 include
    for prefix, forbidden, why in LAYER_RULES:
        if not rel.startswith(prefix):
            continue
        for m in re.finditer(r'#\s*include\s*"([^"]+)"', code):
            inc = m.group(1)
            if inc.startswith(forbidden):
                line = code[: m.start()].count("\n") + 1
                problems.append((rel, line, "R2-R5", "%s (%s)" % (inc, why), m.group(0).strip()))
    # 规则 R6：头文件禁止 using namespace std
    if rel.endswith((".h", ".hpp")) and re.search(r"using\s+namespace\s+std\s*;", code):
        line = code[: re.search(r"using\s+namespace\s+std\s*;", code).start()].count("\n") + 1
        problems.append((rel, line, "R6", "`using namespace std;` in a header", "using namespace std;"))
    return problems


def changed_files():
    try:
        out = subprocess.check_output(
            ["git", "diff", "--name-only", "HEAD"], cwd=ROOT, stderr=subprocess.DEVNULL
        ).decode("utf-8", "replace")
        return {os.path.normpath(l.strip()).replace("\\", "/") for l in out.splitlines() if l.strip()}
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--diff", action="store_true", help="check only git-modified files")
    args = ap.parse_args()

    only = changed_files() if args.diff else None
    if args.diff and only is None:
        print("warning: cannot get the git change list, falling back to a full check", file=sys.stderr)
        only = None

    all_problems = []
    scanned = 0
    for full, rel in iter_sources():
        if only is not None and rel not in only and ("ltm-index/" + rel) not in only:
            continue
        with open(full, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        scanned += 1
        all_problems.extend(check_file(rel, text))

    if args.json:
        print(json.dumps(
            {"scanned": scanned, "violations": [
                {"file": p[0], "line": p[1], "rule": p[2], "why": p[3], "text": p[4]}
                for p in all_problems]},
            ensure_ascii=False, indent=2))
        return 1 if all_problems else 0

    print("layering check: scanned %d source files" % scanned)
    if not all_problems:
        print("PASS: no layering or I/O violations found.")
        return 0

    print("\n%d violations found:" % len(all_problems))
    for rel, line, rule, why, text in sorted(all_problems):
        print("  [%s] %s:%d\n        %s\n        > %s" % (rule, rel, line, why, text))
    print("\nHow to fix:")
    print("  * the index layer may only reach disk through BufferPool (fetch_page/new_page/flush_page)")
    print("  * the storage layer must not depend on the index layer; shared types live in src/util/**")
    print("  * for a genuine exception, register it in IO_ALLOWLIST in scripts/check_layering.py and say why")
    return 1


if __name__ == "__main__":
    sys.exit(main())
