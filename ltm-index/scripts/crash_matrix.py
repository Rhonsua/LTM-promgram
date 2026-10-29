#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""崩溃恢复矩阵驱动（方案 §5.2 守门程序 2）。

判据（方案 §5.2）：恢复后的全量遍历必须**逐条等于**崩溃前已提交的状态，
且 `validate_tree` 全绿（不变量 I1..I10）。"已提交"由被测程序自己写的旁路日志
（journal.txt）定义，恢复之后逐条比对 —— 判据不依赖被测程序的任何自述。

每个用例跑三个进程（本机项目路径含非 ASCII 字符，由 Python 用 Unicode 传参，
不经过命令行编码转换）：
    1) --phase=work     跑负载并在注入点强杀（退出码 9）或跑完（退出码 0）
    2) --phase=corrupt  仅 corrupt_page / tear_page：在页文件里制造损坏
    3) --phase=verify   恢复 + 全量遍历 + 与 journal 比对

前置条件：
    python scripts/build_local.py fi          # 配置并编译 build-fi/（-DLTM_ENABLE_FAULT_INJECT=ON）
    或   scripts\\build.bat faultinject

用法：
    python scripts/crash_matrix.py --list
    python scripts/crash_matrix.py --dry-run
    python scripts/crash_matrix.py                 # 真正跑（需要 build-fi）
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (名字, 说明, 参数列表, 适用的引擎)  —— 与 tests/crash/crash_test.cc 的 --inject / --engine 一一对应
#
# 两个引擎跑同一套判据（"恢复后的全量遍历 == 旁路日志里的已提交集合"）：
#   bplus  = 单棵 B+ 树（M4-3 的验收对象）
#   hybrid = HybridIndex（M5）：同一条判据额外覆盖 I8 —— 两棵树要么都生效、要么都回滚
# corrupt_page / tear_page 只跑 bplus：它们的现场需要"页已落盘"，而 hybrid 引擎
# 没有"刷盘但不标干净关闭"的钩子（见 crash_test.cc 的 Engine::supports_unclean_end）。
INJECTION_POINTS = [
    ("wal_append", "the log was appended but the txn is not committed yet",
     [1, 5, 40, 300, 900], ["bplus", "hybrid"]),
    ("before_flush", "a dirty page is about to be written", [1, 2, 5], ["bplus", "hybrid"]),
    ("checkpoint_stage", "the four checkpoint phases (begin / flush / end / truncate)",
     [1, 2, 3, 4], ["bplus", "hybrid"]),
    ("corrupt_page", "some bytes of an already-flushed page are flipped", [0], ["bplus"]),
    ("tear_page", "only the first half of the page was written (torn write)", [0], ["bplus"]),
]

WORKLOADS = ["sequential", "random", "mixed"]

# 需要先制造损坏的两个注入点（以及它们对应的损坏方式）
CORRUPT_MODES = {"corrupt_page": "flip", "tear_page": "tear"}

CRASH_EXIT_CODE = 9  # FaultInjector::Die() 用 _exit(9)


def find_exe(name, build_dir="build-fi"):
    for sub in ("", "RelWithDebInfo", "Release", "Debug"):
        for ext in (".exe", ""):
            p = os.path.join(ROOT, build_dir, sub, name + ext)
            if os.path.isfile(p):
                return p
    return None


def run_case(exe, workdir, inject, param, workload, checkpoint_every, engine):
    args = ["--phase=work", "--inject=" + inject, "--workload=" + workload, "--n=%d" % param,
            "--engine=" + engine, "--dir=" + workdir]
    if checkpoint_every:
        args.append("--checkpoint-every=%d" % checkpoint_every)
    if inject in CORRUPT_MODES:
        args.append("--end=unclean")
    raw = []
    r = subprocess.run([exe] + args, cwd=ROOT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    raw.append("$ ltm_crash_test " + " ".join(args))
    raw.append(r.stdout.strip())
    if r.stderr.strip():
        raw.append("[stderr] " + r.stderr.strip())
    raw.append("exit=%d" % r.returncode)
    if r.returncode not in (0, CRASH_EXIT_CODE):
        return {"verdict": "FAIL (work phase exit %d)" % r.returncode, "raw": raw,
                "crashed": "?", "recovery_us": None}

    crashed = "yes" if r.returncode == CRASH_EXIT_CODE else "no"

    if inject in CORRUPT_MODES:
        cargs = ["--phase=corrupt", "--corrupt-mode=" + CORRUPT_MODES[inject], "--engine=" + engine,
                 "--dir=" + workdir]
        c = subprocess.run([exe] + cargs, cwd=ROOT, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        raw.append("$ ltm_crash_test " + " ".join(cargs))
        raw.append(c.stdout.strip())
        raw.append("exit=%d" % c.returncode)
        if c.returncode != 0:
            return {"verdict": "FAIL (corrupt phase exit %d)" % c.returncode, "raw": raw,
                    "crashed": crashed, "recovery_us": None}

    vargs = ["--phase=verify", "--engine=" + engine, "--dir=" + workdir]
    if inject in CORRUPT_MODES:
        vargs.append("--expect-repair")
    v = subprocess.run([exe] + vargs, cwd=ROOT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    raw.append("$ ltm_crash_test " + " ".join(vargs))
    raw.append(v.stdout.strip())
    if v.stderr.strip():
        raw.append("[stderr] " + v.stderr.strip())
    raw.append("exit=%d" % v.returncode)

    recovery_us = None
    m = re.search(r"recovery_us=(\d+)", v.stdout)
    if m:
        recovery_us = int(m.group(1))
    ok = v.returncode == 0 and "VERIFY_OK" in v.stdout
    return {"verdict": "PASS" if ok else "FAIL (verify exit %d)" % v.returncode,
            "raw": raw, "crashed": crashed, "recovery_us": recovery_us}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--build-dir", default="build-fi")
    ap.add_argument("--only", default="", help="only run injection points whose name contains this")
    ap.add_argument("--ops", type=int, default=300)
    args = ap.parse_args()

    total = sum(len(ns) * len(engines) for _, _, ns, engines in INJECTION_POINTS) * len(WORKLOADS)
    if args.list:
        for name, when, ns, engines in INJECTION_POINTS:
            print("%-18s %s" % (name, when))
            print("%-18s n/stage in %s   engines: %s" % ("", ns, ",".join(engines)))
        print("\nworkloads: %s" % WORKLOADS)
        print("total cases: %d" % total)
        return 0

    out_dir = os.path.join(ROOT, "report")
    evidence_dir = os.path.join(out_dir, "m4_3")
    os.makedirs(evidence_dir, exist_ok=True)
    md_path = os.path.join(out_dir, "recovery_matrix.md")
    raw_path = os.path.join(evidence_dir, "crash_matrix_raw.txt")

    exe = find_exe("ltm_crash_test", args.build_dir)
    lines = []
    lines.append("# crash recovery matrix\n")
    lines.append("generated by `scripts/crash_matrix.py`; do not edit by hand.\n")
    lines.append("criterion (design doc 5.2): after recovery, a full traversal equals the **committed**")
    lines.append("state recorded in the side journal, and `Validate()` is all green (invariants I1..I10).")
    lines.append("")
    lines.append("`engine=bplus` runs a single B+ tree; `engine=hybrid` runs the hybrid index, so the")
    lines.append("same criterion also covers I8 -- both trees must come back, or neither.")
    lines.append("")
    lines.append("`crashed=no` means the injection point was never reached in that run (the process")
    lines.append("finished normally) -- the case is still verified, it just did not exercise a crash.")
    lines.append("")
    lines.append("| injection point | params | engine | workload | crashed | recovery (us) | committed intact | verdict |")
    lines.append("| --- | --- | --- | --- | --- | --- | --- | --- |")

    raw_all = []
    n_pass = 0
    n_fail = 0
    n_crash = 0
    for name, _when, params, engines in INJECTION_POINTS:
        if args.only and args.only not in name:
            continue
        for param in params:
            for engine in engines:
                for wl in WORKLOADS:
                    if exe is None or args.dry_run:
                        lines.append("| %s | n=%s | %s | %s | -- | -- | -- | pending (no build-fi) |"
                                     % (name, param, engine, wl))
                        continue
                    workdir = os.path.join(ROOT, "log", "crash_%s_%s_%s_%s" % (name, param, engine, wl))
                    shutil.rmtree(workdir, ignore_errors=True)
                    ckpt_every = 40 if name == "checkpoint_stage" else 0
                    res = run_case(exe, workdir, name, param, wl, ckpt_every, engine)
                    raw_all.append("=== case: %s n=%s engine=%s workload=%s ===" % (name, param, engine, wl))
                    raw_all.extend(res["raw"])
                    raw_all.append("")
                    if res["crashed"] == "yes":
                        n_crash += 1
                    if res["verdict"] == "PASS":
                        n_pass += 1
                    else:
                        n_fail += 1
                    rec = "--" if res["recovery_us"] is None else "%d" % res["recovery_us"]
                    lines.append("| %s | n=%s | %s | %s | %s | %s | %s | %s |"
                                 % (name, param, engine, wl, res["crashed"], rec,
                                    "yes" if res["verdict"] == "PASS" else "?",
                                    res["verdict"]))
                    # 每个用例的日志可达数 MB（一次 300 操作的日志 ~3 MB）。原始输出已经落在
                    # crash_matrix_raw.txt 里，工作目录本身没有保留价值。
                    if res["verdict"] == "PASS":
                        shutil.rmtree(workdir, ignore_errors=True)

    lines.append("")
    lines.append("## summary\n")
    lines.append("- total cases: %d" % total)
    lines.append("- passed: %d, failed: %d" % (n_pass, n_fail))
    lines.append("- cases where the injected crash actually fired: %d" % n_crash)
    lines.append("- executable: %s" % (exe or "not found (run `python scripts/build_local.py fi` first)"))
    lines.append("- raw per-case output: `report/m4_3/crash_matrix_raw.txt`")

    with open(md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    if raw_all:
        with open(raw_path, "w", encoding="utf-8") as f:
            f.write("\n".join(raw_all))

    print("matrix written to %s" % os.path.relpath(md_path, ROOT))
    print("cases: %d passed, %d failed, %d real crashes" % (n_pass, n_fail, n_crash))
    if exe is None:
        print("note: no ltm_crash_test under %s; run `python scripts/build_local.py fi` first."
              % args.build_dir)
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
