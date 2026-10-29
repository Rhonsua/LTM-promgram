#!/usr/bin/env python3
"""Build and retain reproducible C1 ASAN/Debug acceptance evidence."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess

import build_local

ROOT = Path(build_local.PROJECT_ROOT)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--mode", choices=("asan", "debug"), required=True)
    parser.add_argument("--output-dir", default="report/c1/acceptance")
    args = parser.parse_args()
    out = (ROOT / args.output_dir / args.mode).resolve()
    out.relative_to(ROOT.resolve())
    out.mkdir(parents=True, exist_ok=True)
    env, cmake, ninja = build_local.build_env()
    bd = ROOT / ("build-asan" if args.mode == "asan" else "build-dbg")
    config = "RelWithDebInfo" if args.mode == "asan" else "Debug"
    manifest = {
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "mode": args.mode, "config": config, "cmake": cmake,
        "cmake_sha256": hashlib.sha256((ROOT / "CMakeLists.txt").read_bytes()).hexdigest(),
        "source_sha256": {
            str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for folder in ("src", "tests", "scripts")
            for p in sorted((ROOT / folder).rglob("*"))
            if p.suffix in (".h", ".cc", ".py")
        }, "runs": [], "passed": False,
    }

    def run(name, command, cwd=ROOT, timeout=1800, death=False):
        print("running " + name, flush=True)
        record = {"name": name, "command": list(map(str, command)), "cwd": str(cwd)}
        try:
            result = subprocess.run(command, cwd=cwd, env=env, capture_output=True,
                                    timeout=timeout)
            raw = result.stdout + b"\n--- stderr ---\n" + result.stderr
            (out / (name + ".txt")).write_bytes(raw)
            record["exit_code"] = result.returncode
            if death:
                expected = 3 if os.name == "nt" else -signal.SIGABRT
                record["passed"] = (
                    result.returncode == expected
                    and b"ORDER_ABORT_PROBE: held=10/L0 requested=20/L2" in raw
                    and b"requested: pid=20" in raw
                    and b"unexpectedly survived" not in raw)
            else:
                record["passed"] = result.returncode == 0
        except subprocess.TimeoutExpired as exc:
            (out / (name + ".txt")).write_bytes((exc.stdout or b"") + (exc.stderr or b""))
            record.update(passed=False, error="timeout")
        manifest["runs"].append(record)
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        if not record["passed"]:
            raise RuntimeError(name + " failed; see " + str(out))

    try:
        run("configure", [cmake, "-S", str(ROOT), "-B", str(bd), "-G", "Ninja",
                          "-DCMAKE_MAKE_PROGRAM=" + ninja,
                          "-DCMAKE_BUILD_TYPE=" + config,
                          "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl",
                          "-DLTM_ENABLE_ASAN=" + ("ON" if args.mode == "asan" else "OFF")])
        run("build", [cmake, "--build", str(bd)])
        run("header_dependencies", [ninja, "-C", str(bd), "-t", "deps",
                                    "CMakeFiles/test_page_latch.dir/tests/concurrency/test_page_latch.cc.obj"])
        if b"test_harness.h" not in (out / "header_dependencies.txt").read_bytes():
            raise RuntimeError("Ninja failed to record the test harness header dependency")
        if args.mode == "asan":
            # Check actual executable instrumentation, not just a CMake flag.
            proof = b"clang_rt.asan" in (bd / "test_status.exe").read_bytes()
            manifest["asan_import_present"] = proof
            if not proof:
                raise RuntimeError("ASAN runtime import missing")
        ctest = str(Path(cmake).with_name("ctest.exe"))
        tests = [ctest, "--output-on-failure"]
        if args.mode == "debug":
            # Debug acceptance tests lock correctness; the long read-scaling
            # measurement is already included in the full ASAN suite.
            tests += ["-R", "^(test_page_latch|test_c1_replay|test_hybrid_diff|test_hybrid_concurrency|test_write_abort)$"]
            manifest["ctest_scope"] = "C1 lock/replay/hybrid/rollback correctness"
        else:
            tests += ["-E", "validate"]
            manifest["ctest_scope"] = "all ordinary tests"
        run("ctest", tests, bd)
        run("validate_bplus", [str(bd / "ltm_validate_bplus.exe"), "--quiet"], bd)
        run("validate_rtree", [str(bd / "ltm_validate_rtree.exe"), "--quiet"], bd)
        if args.mode == "debug":
            run("order_abort", [str(bd / "test_page_latch.exe"), "--probe-order-abort"],
                bd, timeout=30, death=True)
        manifest["passed"] = True
    except (RuntimeError, OSError) as exc:
        manifest["error"] = str(exc)
        print(str(exc), flush=True)
    finally:
        manifest["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print("C1_ACCEPTANCE: " + ("passed" if manifest["passed"] else "FAILED"))
    return 0 if manifest["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
