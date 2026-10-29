#!/usr/bin/env python3
"""Build, write/checkpoint/close, reopen in a fresh process, verify, and retain evidence."""
import argparse
import datetime
import hashlib
import json
import math
import os
import shutil
from pathlib import Path
import subprocess
import sys
import time
import build_local

ROOT = Path(__file__).resolve().parents[1]
INTS = ("schema_version", "records", "pool_frames", "seed", "point_checked", "missing_checked",
        "full_scan_rows", "range_rows", "spatial_rows", "contain_rows", "empty_rows", "knn_rows",
        "i8_pairs_checked", "working_set_pages", "logical_writes", "page_reads", "page_writes",
        "wal_bytes", "evictions", "i6_violations", "lock_violations")


def parse_result(stdout, code, stage):
    if code != 0: raise ValueError("process exited with code %d" % code)
    lines = [line[16:] for line in stdout.splitlines() if line.startswith("PIPELINE_RESULT ")]
    if len(lines) != 1: raise ValueError("expected exactly one pipeline result")
    row = json.loads(lines[0])
    if not isinstance(row, dict) or any(type(row.get(key)) is not int or row[key] < 0 for key in INTS):
        raise ValueError("missing or invalid pipeline counters")
    expected = dict(schema_version=1, stage=stage, records=512, pool_frames=12, seed=42,
                    point_checked=512, missing_checked=1, full_scan_rows=512, range_rows=65,
                    empty_rows=0, knn_rows=10, i8_pairs_checked=512, i6_violations=0,
                    lock_violations=0, logical_writes=512 if stage == "load" else 0)
    if row.get("correct") is not True or any(row.get(key) != value for key, value in expected.items()):
        raise ValueError("incorrect pipeline result or parameter mismatch")
    if not (row["working_set_pages"] > row["pool_frames"] and row["evictions"] > 0 and row["page_reads"] > 0 and 512 >= row["spatial_rows"] > row["contain_rows"] > 0):
        raise ValueError("missing disk/eviction or spatial-query evidence")
    if stage == "load" and (row["page_writes"] == 0 or row["wal_bytes"] == 0):
        raise ValueError("write stage did not exercise page/WAL writes")
    if not isinstance(row.get("dataset_hash"), str) or not row["dataset_hash"].isdigit():
        raise ValueError("missing logical record fingerprint")
    return row


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument("--build-dir", default="build-ninja")
    ap.add_argument("--output-dir")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--timeout", type=float, default=300)
    args = ap.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0: ap.error("timeout must be positive and finite")
    build = (ROOT / args.build_dir).resolve()
    build.relative_to(ROOT)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    out = (ROOT / (args.output_dir or "report/pipeline/" + stamp)).resolve()
    out.relative_to(ROOT)
    out.mkdir(parents=True, exist_ok=False)
    manifest = {"passed": False, "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "arguments": vars(args), "runs": [], "database": str(out / "database")}

    def save():
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    save()
    try:
        windows = os.name == "nt"
        if windows:
            env, cmake, ninja = build_local.build_env()
        else:
            env = dict(os.environ)
            cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
            if not args.skip_build and (cmake is None or ninja is None):
                raise ValueError("cmake and ninja are required to build the pipeline")
        manifest.update(cmake=cmake, ninja=ninja)

        def run(name, command):
            print("PIPELINE_STAGE: " + name, flush=True)
            tick = time.perf_counter()
            try:
                result = subprocess.run(list(map(str, command)), cwd=ROOT, env=env, capture_output=True, timeout=args.timeout)
                stdout, stderr, code = result.stdout, result.stderr, result.returncode
            except subprocess.TimeoutExpired as exc:
                (out / (name + ".stdout.txt")).write_bytes(exc.stdout or b"")
                (out / (name + ".stderr.txt")).write_bytes(exc.stderr or b"")
                manifest["runs"].append({"stage": name, "command": list(map(str, command)), "error": "timeout", "passed": False})
                save(); raise ValueError(name + " timed out") from exc
            (out / (name + ".stdout.txt")).write_bytes(stdout)
            (out / (name + ".stderr.txt")).write_bytes(stderr)
            manifest["runs"].append({"stage": name, "command": list(map(str, command)), "exit_code": code,
                                     "wall_seconds": time.perf_counter() - tick, "passed": code == 0 and name not in ("load", "verify")})
            save()
            if code != 0: raise ValueError(name + " failed; see " + str(out / (name + ".stderr.txt")))
            return stdout.decode("utf-8"), code

        if not args.skip_build:
            if windows:
                if not (build / "CMakeCache.txt").is_file():
                    run("configure", [sys.executable, ROOT / "scripts/build_local.py", "configure", str(build)])
                run("build", [sys.executable, ROOT / "scripts/build_local.py", "build", str(build)])
            else:
                if not (build / "CMakeCache.txt").is_file():
                    command = [cmake, "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja]
                    if shutil.which("clang") and shutil.which("clang++"):
                        command += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]
                    run("configure", command)
                run("build", [cmake, "--build", build, "--target", "ltm_pipeline", "--parallel", "4"])
        exe = build / ("ltm_pipeline.exe" if windows else "ltm_pipeline")
        if not exe.is_file(): raise ValueError("build ltm_pipeline before running")
        files = [ROOT / "CMakeLists.txt", ROOT / "demo/pipeline_main.cc"] + sorted((ROOT / "scripts").glob("*.py"))
        files += sorted((ROOT / "src").rglob("*.cc")) + sorted((ROOT / "src").rglob("*.h"))
        manifest["source_sha256"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
        manifest["binary_sha256"] = hashlib.sha256(exe.read_bytes()).hexdigest()
        manifest["exe"] = str(exe)
        if (build / "CMakeCache.txt").is_file():
            (out / "CMakeCache.txt").write_bytes((build / "CMakeCache.txt").read_bytes())
        results = []
        for stage in ("load", "verify"):
            stdout, code = run(stage, [exe, "--stage=" + stage, "--run-dir=" + manifest["database"]])
            row = parse_result(stdout, code, stage)
            manifest["runs"][-1]["passed"] = True
            save()
            (out / (stage + ".result.json")).write_text(json.dumps(row, indent=2), encoding="utf-8")
            results.append(row)
        for key in ("dataset_hash", "working_set_pages", "spatial_rows", "contain_rows"):
            if results[0][key] != results[1][key]: raise ValueError("fresh-process result changed: " + key)
        manifest["source_unchanged"] = all(hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == digest for name, digest in manifest["source_sha256"].items())
        manifest["binary_unchanged"] = hashlib.sha256(exe.read_bytes()).hexdigest() == manifest["binary_sha256"]
        if not manifest["source_unchanged"] or not manifest["binary_unchanged"]: raise ValueError("code changed during the pipeline")
        manifest["database_files"] = {name: {"bytes": (out / "database" / name).stat().st_size,
                                                   "sha256": hashlib.sha256((out / "database" / name).read_bytes()).hexdigest()}
                                      for name in ("pages.data", "records.wal")}
        manifest.update(passed=True, results=results)
    except (ValueError, OSError, SystemExit) as exc:
        manifest["error"] = str(exc)
        print("PIPELINE_ERROR: " + str(exc), file=sys.stderr)
    manifest["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save()
    print("PIPELINE_VERDICT: " + ("passed" if manifest["passed"] else "failed"), flush=True)
    print("PIPELINE_REPORT: " + str(out), flush=True)
    return 0 if manifest["passed"] else 1


if __name__ == "__main__": sys.exit(main())
