#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""不变量校验驱动（方案 §5.2 守门程序 1）。

作用：编译并运行 ltm_validate_bplus / ltm_validate_rtree，解析退出码并给出
可读结论，同时把输出落盘到 report/validate_<tree>.txt 作为结题证据。

退出码约定（由 tests/index/test_*_validate.cc 实现）：
    0 = 不变量全部通过
    1 = 真的校验失败（必须立即修）
    2 = 尚未实现（M0..M5 阶段的期望状态）

用法：
    python scripts/validate_tree.py                 # 两棵树都跑
    python scripts/validate_tree.py --tree bplus
    python scripts/validate_tree.py --list          # 只列出会检查什么
    python scripts/validate_tree.py --build-dir build-vs --config RelWithDebInfo
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_cmake():
    """找一个可用的 cmake.exe 绝对路径。

    原来只有一行 `os.environ.get("CMAKE_EXE", "cmake")`：本机 cmake 不在 PATH 上
    （它是 VS 自带的），于是本脚本会以 FileNotFoundError 的 traceback 收场 ——
    报错指向 subprocess，而不是"你没给我 cmake"。守门脚本的失败信息必须能直接指向
    要修的那件事，所以在环境变量之外再补两条兜底：PATH，以及 VS 自带 cmake 的
    常见安装位置。
    """
    explicit = os.environ.get("CMAKE_EXE")
    if explicit:
        return explicit
    on_path = shutil.which("cmake")
    if on_path:
        return on_path
    patterns = [
        r"D:\visual studio\IDE\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"),
                     "Microsoft Visual Studio", "*", "*", "Common7", "IDE",
                     "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe"),
        os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                     "Microsoft Visual Studio", "*", "*", "Common7", "IDE",
                     "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe"),
    ]
    for pat in patterns:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[-1]
    return None

CHECKS = {
    "bplus": {
        "exe": "ltm_validate_bplus",
        "target": "ltm_validate_bplus",
        "invariants": [
            "I1  non-leaf slot i key == the min key of child i",
            "I2  leaf chain next/prev agree bidirectionally and keys are strictly ascending",
            "I3  every non-root page fill >= 50%",
            "I4  all root-to-leaf paths are the same length",
        ],
    },
    "rtree": {
        "exe": "ltm_validate_rtree",
        "target": "ltm_validate_rtree",
        "invariants": [
            "I9  a parent slot MBR is exactly the union of its child's MBRs",
            "I3  every non-root page fill >= 40%",
            "KNN best-first agrees with the brute-force scan (distance tolerance 1e-9)",
        ],
    },
}

EXIT_MEANING = {
    0: "PASS",
    1: "validation failed",
    2: "not implemented yet",
}

# R 树三策略对照（M3 的量化证据之一）。
# 与上表分开的原因：它不是"通过/失败"型的闸门，而是"产出一张可对照的表"。
# 数字由 ltm_validate_rtree 自己写入 CSV，本脚本只负责调用 —— 手工誊抄的表格
# 无法复现，方案 §9.2 明确禁止。
POLICY_CSV = {
    "tree": "rtree",
    "exe": "ltm_validate_rtree",
    "args": ["--policy-csv=report/rtree_split_policy.csv", "--ops=3000",
             "--frames=64", "--quiet"],
    "out": "report/rtree_split_policy.csv",
    "why": "overlap area, build time and query page visits for the three split policies (even / linear / quadratic)",
}


def find_exe(build_dir, name):
    """先找 build/<exe>，再找 build/<cfg>/<exe>。

    本项目的 CMakeLists 把 CMAKE_RUNTIME_OUTPUT_DIRECTORY 设成了 CMAKE_BINARY_DIR，
    所以 Visual Studio 生成器也会把 exe 直接放在 build-vs/ 根目录，而不是
    build-vs/RelWithDebInfo/。两种布局都支持，避免改构建配置后脚本失效。
    """
    suffixes = [".exe", ""] if os.name == "nt" else ["", ".exe"]
    for sub in ("", "RelWithDebInfo", "Release", "Debug", "MinSizeRel"):
        for suf in suffixes:
            cand = os.path.join(ROOT, build_dir, sub, name + suf)
            if os.path.isfile(cand):
                return cand
    return None


def build(build_dir, config, targets):
    cmake = find_cmake()
    if cmake is None:
        print("cannot find cmake.exe. Set CMAKE_EXE to its absolute path, e.g.")
        print('  set CMAKE_EXE=D:\\visual studio\\IDE\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe')
        print("(cmake is not on PATH in this environment: it ships with Visual Studio.)")
        return False
    for t in targets:
        cmd = [cmake, "--build", os.path.join(ROOT, build_dir), "--config", config, "--target", t]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if r.returncode != 0:
            print("build of %s failed:\n%s\n%s" % (t, r.stdout[-2000:], r.stderr[-2000:]))
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", choices=["bplus", "rtree", "all"], default="all")
    ap.add_argument("--build-dir", default="build-vs")
    ap.add_argument("--config", default="RelWithDebInfo")
    ap.add_argument("--list", action="store_true", help="list the checks only")
    ap.add_argument("--no-build", action="store_true", help="skip the build, run the existing executables")
    args = ap.parse_args()

    trees = list(CHECKS) if args.tree == "all" else [args.tree]

    if args.list:
        for t in trees:
            print("%s will check:" % t)
            for inv in CHECKS[t]["invariants"]:
                print("  - %s" % inv)
        return 0

    out_dir = os.path.join(ROOT, "report")
    os.makedirs(out_dir, exist_ok=True)

    if not args.no_build:
        if not build(args.build_dir, args.config, [CHECKS[t]["target"] for t in trees]):
            return 1

    results = {}
    for t in trees:
        exe = find_exe(args.build_dir, CHECKS[t]["exe"])
        if exe is None:
            print("[%s] executable not found: %s (build first)" % (t, CHECKS[t]["exe"]))
            results[t] = ("missing", -1, "", 0.0)
            continue
        t0 = time.time()
        r = subprocess.run([exe], capture_output=True, text=True, encoding="utf-8", errors="replace")
        dt = time.time() - t0
        out = (r.stdout or "") + (r.stderr or "")
        results[t] = (EXIT_MEANING.get(r.returncode, "unknown exit code %d" % r.returncode),
                      r.returncode, out, dt)
        with open(os.path.join(out_dir, "validate_%s.txt" % t), "w", encoding="utf-8") as f:
            f.write("# ltm_index invariant validation -- %s\n" % t)
            f.write("# exit code %d (%s), took %.3f s\n" % (r.returncode, results[t][0], dt))
            f.write("# checks:\n")
            for inv in CHECKS[t]["invariants"]:
                f.write("#   %s\n" % inv)
            f.write("\n" + out)

    print("=" * 68)
    print("invariant validation results (evidence in report/validate_*.txt)")
    print("=" * 68)
    worst = 0
    for t, (meaning, code, out, dt) in results.items():
        flag = {0: "PASS", 1: "FAIL", 2: "TODO", -1: "MISS"}.get(code, "????")
        print("[%s] %-10s exit=%d took=%.3fs" % (flag, t, code, dt))
        if code == 2:
            print("      not implemented yet -- the expected state, see the milestones in docs/status.md")
        elif code == 1:
            print("      validation failed, output follows:")
            for line in out.strip().splitlines()[-15:]:
                print("      | " + line)
            worst = 1
        elif code == -1:
            worst = max(worst, 1)

    # 三策略对照：只在对应树的不变量自检通过后才有意义（树都没建对，比策略无意义）
    src = results.get(POLICY_CSV["tree"])
    if src is not None and src[1] == 0:
        exe = find_exe(args.build_dir, POLICY_CSV["exe"])
        print("-" * 68)
        print("three-policy comparison: %s" % POLICY_CSV["why"])
        if exe is None:
            print("[MISS] executable not found: %s" % POLICY_CSV["exe"])
            worst = max(worst, 1)
        else:
            r = subprocess.run([exe] + POLICY_CSV["args"], cwd=ROOT, capture_output=True,
                               text=True, encoding="utf-8", errors="replace")
            for line in (r.stdout or "").strip().splitlines():
                print("      | " + line)
            if r.returncode == 0:
                print("[PASS] evidence written to %s" % POLICY_CSV["out"])
            else:
                print("[FAIL] exit code %d" % r.returncode)
                worst = max(worst, 1)
    return worst


if __name__ == "__main__":
    sys.exit(main())
