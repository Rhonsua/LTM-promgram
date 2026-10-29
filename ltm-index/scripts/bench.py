#!/usr/bin/env python3
"""Checked B+/Hybrid resident/evicting comparisons with independent result validation."""
import argparse
import csv
import datetime
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
COUNTERS = ("hits", "misses", "evictions", "dirty_evictions", "page_reads", "page_writes", "wal_bytes", "result_rows", "working_set_pages", "height", "i6_violations", "lock_violations")
FLOATS = ("throughput_ops_s", "measurement_seconds", "measured_wall_seconds", "load_seconds", "p50_us", "p95_us", "p99_us", "p999_us", "hit_rate")
IDENTITY = ("schema_version", "engine", "threads", "workload", "seed", "records", "ops", "warmup_ops", "range_width", "pool_frames", "page_size", "regime", "dataset_hash", "query_hash", "correct", "order_capacity", "heat", "policy", "config_dump", "run_dir")
FIELDS = IDENTITY + COUNTERS + FLOATS
HYBRID_INTS = ("spatial_width", "knn_k", "mbr_dim", "active_pages", "bplus_pages", "rtree_pages", "i8_pairs_checked")
HYBRID_FIELDS = HYBRID_INTS + ("split_policy", "oracle_seconds")
SPATIAL_WORKLOADS = ("spatial", "contain", "knn")


def parse_result(stdout, exit_code, expected):
    if exit_code != 0:
        raise ValueError("benchmark exited with code %d" % exit_code)
    lines = [line[len("BENCH_RESULT "):] for line in stdout.splitlines() if line.startswith("BENCH_RESULT ")]
    if len(lines) != 1:
        raise ValueError("expected exactly one structured benchmark result")
    row = json.loads(lines[0])
    if not isinstance(row, dict) or any(key not in row for key in FIELDS):
        raise ValueError("missing required metrics")
    for key in COUNTERS + ("schema_version", "threads", "seed", "records", "ops", "warmup_ops", "range_width", "pool_frames", "page_size", "order_capacity"):
        if type(row[key]) is not int or row[key] < 0:
            raise ValueError("invalid integer metric: " + key)
    for key in FLOATS:
        if type(row[key]) not in (int, float) or not math.isfinite(row[key]) or row[key] < 0:
            raise ValueError("invalid finite metric: " + key)
    for key, value in expected.items():
        if row.get(key) != value:
            raise ValueError("parameter mismatch: " + key)
    for key in ("engine", "workload", "regime", "dataset_hash", "query_hash", "heat", "policy", "config_dump", "run_dir"):
        if not isinstance(row[key], str) or not row[key]:
            raise ValueError("invalid string metric: " + key)
    hybrid = row["workload"] in SPATIAL_WORKLOADS
    if hybrid:
        if any(key not in row for key in HYBRID_FIELDS):
            raise ValueError("missing Hybrid metrics")
        for key in HYBRID_INTS:
            if type(row[key]) is not int or row[key] <= 0:
                raise ValueError("invalid Hybrid integer: " + key)
        if type(row["oracle_seconds"]) not in (int, float) or not math.isfinite(row["oracle_seconds"]) or row["oracle_seconds"] < 0:
            raise ValueError("invalid oracle duration")
        if row["mbr_dim"] != 2 or row["split_policy"] != "quadratic" or row["knn_k"] > row["records"] or row["spatial_width"] > 1000000:
            raise ValueError("unsupported spatial configuration")
        if row["i8_pairs_checked"] != row["records"] or row["working_set_pages"] != row["bplus_pages"] + row["rtree_pages"] + 1 or row["active_pages"] != row["rtree_pages"] + 1:
            raise ValueError("invalid Hybrid inventory or I8 check")
    elif row["workload"] not in ("point", "range"):
        raise ValueError("unsupported workload")
    if (row["heat"], row["policy"]) != (("enabled", "migration") if hybrid else ("none", "clock")) or row["order_capacity"] != 32 or row["page_size"] != 4096:
        raise ValueError("unsupported benchmark configuration")
    if row["schema_version"] != (2 if hybrid else 1) or row["engine"] != ("hybrid" if hybrid else "bplus") or row["threads"] != 1 or row["correct"] is not True:
        raise ValueError("unsupported or incorrect benchmark result")
    if row["ops"] <= 0 or row["records"] <= 0 or row["measurement_seconds"] <= 0 or row["p50_us"] <= 0:
        raise ValueError("zero measured work or duration")
    if row["i6_violations"] != 0 or row["lock_violations"] != 0 or not 0 <= row["hit_rate"] <= 1:
        raise ValueError("invariant violation or invalid hit rate")
    if not row["p50_us"] <= row["p95_us"] <= row["p99_us"] <= row["p999_us"]:
        raise ValueError("unordered percentiles")
    if not math.isclose(row["throughput_ops_s"], row["ops"] / row["measurement_seconds"], rel_tol=1e-6):
        raise ValueError("inconsistent throughput")
    total = row["hits"] + row["misses"]
    if total == 0 or not math.isclose(row["hit_rate"], row["hits"] / total, rel_tol=1e-8, abs_tol=1e-8) or row["measured_wall_seconds"] < row["measurement_seconds"]:
        raise ValueError("inconsistent hit rate or measurement interval")
    if row["page_reads"] != row["misses"] or row["dirty_evictions"] > row["evictions"]:
        raise ValueError("inconsistent I/O counters")
    regime = "resident" if row["pool_frames"] >= row["working_set_pages"] else "evicting"
    if hybrid and regime == "evicting" and row["pool_frames"] >= row["active_pages"]:
        raise ValueError("pool lies between active and full inventories; regime is indeterminate")
    if row["working_set_pages"] <= 1 or row["regime"] != regime:
        raise ValueError("unproven working set regime")
    if regime == "resident" and (row["misses"] != 0 or row["evictions"] != 0):
        raise ValueError("resident run did not remain resident")
    if regime == "evicting" and (row["misses"] == 0 or row["evictions"] == 0):
        raise ValueError("evicting run did not exercise eviction")
    if not isinstance(row["dataset_hash"], str) or not row["dataset_hash"].isdigit() or not isinstance(row["query_hash"], str) or not row["query_hash"].isdigit():
        raise ValueError("missing reproducible input hashes")
    if row["workload"] == "point" and row["result_rows"] != row["ops"]:
        raise ValueError("wrong point result count")
    if row["workload"] == "range" and not row["ops"] <= row["result_rows"] <= row["ops"] * row["range_width"]:
        raise ValueError("wrong range result count")
    if row["workload"] == "knn" and row["result_rows"] != row["ops"] * row["knn_k"]:
        raise ValueError("wrong KNN result count")
    if row["workload"] in ("spatial", "contain") and row["result_rows"] > row["ops"] * row["records"]:
        raise ValueError("impossible spatial result count")
    if row["page_writes"] != 0 or row["wal_bytes"] != 0:
        raise ValueError("read-only measurement unexpectedly wrote pages or WAL")
    return row


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument("--experiment", default="mem-vs-ltm")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--records", type=int, default=2048)
    ap.add_argument("--ops", type=int, default=2000)
    ap.add_argument("--warmup-ops", type=int, default=None)
    ap.add_argument("--range-width", type=int, default=16)
    ap.add_argument("--spatial-width", type=int, default=8)
    ap.add_argument("--knn-k", type=int, default=10)
    ap.add_argument("--frames", type=int, nargs="+", default=[12, 512])
    ap.add_argument("--workloads", nargs="+", choices=["point", "range", *SPATIAL_WORKLOADS], default=["point", "range"])
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--build-dir", default="build-ninja")
    ap.add_argument("--output-dir", default=None)
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    if args.list:
        print("mem-vs-ltm: B+ point/range (CLOCK); Hybrid spatial/contain/knn (migration policy)")
        print("mixed: scripts/mixed_bench.py (fixed bounded read/write stream; separate schema)")
        print("deferred: page-size, rtree-split, migration, recovery"); return 0
    if args.experiment != "mem-vs-ltm":
        ap.error("experiment is deferred; only mem-vs-ltm is implemented")
    if not (0 <= args.seed < 2**64 and 0 < args.records <= 200000 and 0 < args.ops <= 1000000 and 0 < args.range_width <= args.records and 0 < args.spatial_width <= 1000000 and 0 < args.knn_k <= args.records and args.repeats >= 3 and all(2 <= f <= 65536 for f in args.frames) and math.isfinite(args.timeout) and args.timeout > 0):
        ap.error("invalid limits; at least three repetitions are required")
    warmup = args.warmup_ops if args.warmup_ops is not None else max(1, args.ops // 10)
    if not 0 <= warmup <= 1000000 or len(set(args.frames)) != len(args.frames) or len(args.frames) < 2 or len(set(args.workloads)) != len(args.workloads):
        ap.error("invalid warmup or duplicate frame count")
    exe = next((ROOT / args.build_dir / sub / name for sub in ("", "RelWithDebInfo", "Release", "Debug") for name in ("ltm_bench.exe", "ltm_bench") if (ROOT / args.build_dir / sub / name).is_file()), None)
    if exe is None: ap.error("build ltm_bench before running")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    out = (ROOT / (args.output_dir or "report/bench/" + stamp)).resolve()
    out.relative_to(ROOT)
    if not args.dry_run: out.mkdir(parents=True, exist_ok=False)
    files = [ROOT / "CMakeLists.txt"] + sorted((ROOT / "src").rglob("*.cc")) + sorted((ROOT / "src").rglob("*.h")) + sorted((ROOT / "scripts").glob("*.py"))
    manifest = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "arguments": vars(args), "exe": str(exe), "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "source_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}, "runs": [], "passed": False}
    rows, hashes = [], {}; failed = False
    for workload in args.workloads:
        for frames in args.frames:
            for repeat in range(1, args.repeats + 1):
                run_dir = out / ("%s_frames%d_repeat%d" % (workload, frames, repeat))
                expected = {"workload": workload, "seed": args.seed, "records": args.records, "ops": args.ops, "warmup_ops": warmup, "range_width": args.range_width, "pool_frames": frames, "page_size": 4096}
                cmd = [str(exe), "--run-dir=" + str(run_dir)] + ["--%s=%s" % (key, val) for key, val in (("workload", workload), ("seed", args.seed), ("records", args.records), ("bench.ops", args.ops), ("warmup-ops", warmup), ("range-width", args.range_width), ("pool.pool_frames", frames))]
                if workload in SPATIAL_WORKLOADS:
                    expected.update(spatial_width=args.spatial_width, knn_k=args.knn_k)
                    cmd += ["--spatial-width=%d" % args.spatial_width, "--knn-k=%d" % args.knn_k]
                if args.dry_run: print(subprocess.list2cmdline(cmd)); continue
                record = {"command": cmd, "repeat": repeat, "workload": workload, "frames": frames, "passed": False}
                tick = time.perf_counter()
                try:
                    run = subprocess.run(cmd, cwd=ROOT, capture_output=True, timeout=args.timeout)
                    run_dir.mkdir(parents=True, exist_ok=True)
                    (run_dir / "stdout.txt").write_bytes(run.stdout); (run_dir / "stderr.txt").write_bytes(run.stderr)
                    record["exit_code"] = run.returncode
                    row = parse_result(run.stdout.decode("utf-8"), run.returncode, expected)
                    if Path(row["run_dir"]).resolve() != run_dir: raise ValueError("run directory mismatch")
                    pair = row["dataset_hash"], row["query_hash"], row["working_set_pages"]
                    if workload in hashes and hashes[workload] != pair: raise ValueError("input or tree shape changed between runs")
                    hashes[workload] = pair
                    row.update(experiment=args.experiment, repeat=repeat, wall_seconds=time.perf_counter() - tick)
                    rows.append(row); record["passed"] = True
                    (run_dir / "result.json").write_text(json.dumps(row, indent=2), encoding="utf-8")
                    print("[OK] %s frames=%d repeat=%d regime=%s" % (workload, frames, repeat, row["regime"]), flush=True)
                except subprocess.TimeoutExpired as exc:
                    run_dir.mkdir(parents=True, exist_ok=True)
                    (run_dir / "stdout.txt").write_bytes(exc.stdout or b""); (run_dir / "stderr.txt").write_bytes(exc.stderr or b"")
                    record["error"] = "timeout"; failed = True
                except (ValueError, OSError) as exc:
                    record["error"] = str(exc); failed = True
                record["wall_seconds"] = time.perf_counter() - tick
                manifest["runs"].append(record)
                (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    if args.dry_run: return 0
    with (out / "results.csv").open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=("experiment", "repeat", "wall_seconds") + FIELDS + HYBRID_FIELDS, extrasaction="ignore")
        writer.writeheader(); writer.writerows(rows)
    summary = []
    for workload in args.workloads:
        for frames in args.frames:
            group = [r for r in rows if r["workload"] == workload and r["pool_frames"] == frames]
            if len(group) != args.repeats: failed = True; continue
            rates = [r["throughput_ops_s"] for r in group]
            summary.append({"workload": workload, "frames": frames, "regime": group[0]["regime"], "repetitions": len(group), "throughput_min": min(rates), "throughput_median": statistics.median(rates), "throughput_max": max(rates), "p99_median_us": statistics.median(r["p99_us"] for r in group), "page_reads_median": statistics.median(r["page_reads"] for r in group)})
    manifest.update(passed=not failed, finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    for workload in args.workloads:
        if {r["regime"] for r in rows if r["workload"] == workload} != {"resident", "evicting"}:
            manifest["passed"] = False
            manifest["comparison_error"] = "comparison must contain both proven regimes"
    manifest["source_unchanged"] = all(hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == digest for name, digest in manifest["source_sha256"].items()) and hashlib.sha256(exe.read_bytes()).hexdigest() == manifest["binary_sha256"]
    manifest["passed"] = manifest["passed"] and manifest["source_unchanged"]
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    (out / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print("BENCH_VERDICT: " + ("passed" if manifest["passed"] else "failed"), flush=True)
    return 0 if manifest["passed"] else 1


if __name__ == "__main__": sys.exit(main())
