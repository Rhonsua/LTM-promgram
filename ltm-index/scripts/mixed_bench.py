"""Fixed mixed workload with checked per-call metrics and fresh-process reopen."""
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
RATIOS = dict(get=35, range=15, spatial=10, contain=5, knn=5, put=15, erase=15)
COUNTERS = ("hits", "misses", "evictions", "dirty_evictions", "page_reads", "page_writes", "wal_bytes",
            "wal_appends", "fsyncs", "pages_created", "pages_allocated", "pages_freed", "logical_writes")
IDENTITY_INTS = ("schema_version", "threads", "seed", "records", "ops", "warmup_ops", "range_width", "spatial_width",
                 "knn_k", "pool_frames", "page_size", "mbr_dim", "order_capacity", "final_records", "peak_records",
                 "record_bound", "i8_pairs_checked", "point_keys_checked", "final_scan_rows", "missing_keys_checked",
                 "spatial_queries_checked", "i6_violations", "lock_violations")
HASHES = ("dataset_hash", "operation_hash", "query_hash", "final_hash")
FIELDS = IDENTITY_INTS + HASHES + ("phase", "workload", "engine", "split_policy", "heat", "policy", "correct",
        "run_dir", "config_dump", "measurement_scope", "generation_seconds")
RUN_FIELDS = ("regime", "initial_working_set_pages", "peak_physical_pages", "load_seconds", "oracle_seconds", "snapshot_seconds",
    "api_seconds", "probe_seconds", "measured_wall_seconds", "throughput_api_ops_s", "throughput_wall_ops_s", "verification_seconds",
    "checkpoint_seconds", "put_new", "put_update", "put_reinsert", "erase_found", "erase_missing", "api_io", "probe_io", "measurement_io",
    "checkpoint_io", "by_type", "samples")


def integer(n):
    return type(n) is int and n >= 0


def finite(n):
    return type(n) in (float, int) and math.isfinite(n) and n >= 0


def close(a, b):
    return math.isclose(a, b, rel_tol=1e-8, abs_tol=1e-10)


def counters(row):
    if not isinstance(row, dict) or any(not integer(row.get(k)) for k in COUNTERS):
        raise ValueError("missing or invalid I/O counters")
    if row["dirty_evictions"] > row["evictions"]:
        raise ValueError("dirty eviction count exceeds total")


def total(samples, field):
    return {k: sum(s[field][k] for s in samples) for k in COUNTERS}


def parse_result(stdout, exit_code, expected, *, trajectory="checked"):
    if trajectory not in ("checked", "measure"):
        raise ValueError("unsupported trajectory")
    with_probes = trajectory == "checked"
    if exit_code != 0:
        raise ValueError("mixed benchmark exited with code %s" % exit_code)
    lines = [s[len("MIXED_RESULT "):] for s in stdout.splitlines() if s.startswith("MIXED_RESULT ")]
    if len(lines) != 1:
        raise ValueError("expected exactly one mixed result")
    row = json.loads(lines[0])
    if not isinstance(row, dict) or any(k not in row for k in FIELDS):
        raise ValueError("missing mixed identity fields")
    if any(not integer(row[k]) for k in IDENTITY_INTS) or not finite(row["generation_seconds"]):
        raise ValueError("invalid identity numbers")
    for k, value in expected.items():
        if row.get(k) != value:
            raise ValueError("parameter mismatch: " + k)
    fixed = dict(schema_version=1 if with_probes else 2, threads=1, workload="mixed", engine="hybrid", mbr_dim=2, page_size=4096,
        order_capacity=32, split_policy="quadratic", heat="enabled", policy="migration",
        measurement_scope="api_calls_with_interleaved_probes" if with_probes else "api_calls_without_write_probes",
        i6_violations=0, lock_violations=0, spatial_queries_checked=3)
    if row["correct"] is not True or any(row[k] != v for k, v in fixed.items()):
        raise ValueError("incorrect or unsupported mixed configuration")
    if not with_probes or "trajectory" in row:
        required = dict(trajectory=trajectory, read_validation="inline_full_record_oracle", stats_sampling="per_api_snapshots",
                        query_counter_semantics="successful_gets_and_all_query_calls")
        probes = row.get("write_probe_calls")
        expected_probes = row["ops"] // 100 * 30 if with_probes and row["phase"] == "run" else 0
        if any(row.get(key) != value for key, value in required.items()) or not integer(probes) or probes != expected_probes:
            raise ValueError("trajectory or write-probe declaration mismatch")
    if not (32 <= row["records"] <= 20000 and 100 <= row["ops"] <= 100000 and row["ops"] % 100 == 0 and
        row["seed"] < 2**64 and 4 <= row["pool_frames"] <= 65536 and row["warmup_ops"] <= 100000 and
        0 < row["range_width"] <= row["records"] and 0 < row["knn_k"] <= row["records"] // 2 and 0 < row["spatial_width"] <= 1000000):
        raise ValueError("invalid parameter limits")
    if not all(isinstance(row[k], str) and row[k] and row[k].isascii() and row[k].isdigit()
        and int(row[k]) < 2**64 and str(int(row[k])) == row[k] for k in HASHES):
        raise ValueError("invalid fingerprint")
    if not all(isinstance(row[k], str) and row[k] for k in ("run_dir", "config_dump")):
        raise ValueError("invalid run directory or config dump")
    bound = row["records"] + row["records"] // 4
    if not (row["record_bound"] == bound and row["records"] <= row["peak_records"] <= bound and
        row["records"] // 2 <= row["final_records"] <= row["peak_records"] and row["i8_pairs_checked"] == row["final_records"] and
        row["final_scan_rows"] == row["final_records"] and row["point_keys_checked"] == bound and
        row["missing_keys_checked"] == bound - row["final_records"]):
        raise ValueError("invalid final inventory")
    if row["phase"] == "verify":
        if not finite(row.get("verification_seconds")) or row["verification_seconds"] <= 0:
            raise ValueError("missing verification duration")
        counters(row.get("readonly_io"))
        if any(row["readonly_io"][k] != 0 for k in ("wal_bytes", "wal_appends", "page_writes", "logical_writes")):
            raise ValueError("reopen check wrote data")
        return row
    if row["phase"] != "run" or any(k not in row for k in RUN_FIELDS):
        raise ValueError("missing measurement fields or invalid phase")
    if not with_probes or "measured_query_calls" in row:
        calls = row.get("measured_query_calls")
        missing_gets = row.get("uncounted_not_found_gets")
        requests = row.get("measured_read_requests")
        if (not all(integer(n) for n in (calls, missing_gets, requests)) or requests != calls + missing_gets
            or requests != row["ops"] // 100 * (100 if with_probes else 70)):
            raise ValueError("extra or missing measured index read calls")
    floats = ("load_seconds", "oracle_seconds", "snapshot_seconds", "api_seconds", "probe_seconds", "measured_wall_seconds",
        "throughput_api_ops_s", "throughput_wall_ops_s", "verification_seconds", "checkpoint_seconds")
    ints = ("initial_working_set_pages", "peak_physical_pages", "put_new", "put_update", "put_reinsert", "erase_found", "erase_missing")
    if any(not finite(row[k]) for k in floats) or any(not integer(row[k]) for k in ints):
        raise ValueError("invalid measurement metrics")
    for name in ("api_io", "probe_io", "measurement_io", "checkpoint_io"):
        counters(row[name])
    samples = row["samples"]
    if not isinstance(samples, list) or len(samples) != row["ops"] or not isinstance(row["by_type"], dict) or set(row["by_type"]) != set(RATIOS):
        raise ValueError("missing samples or operation types")
    live, erased = set(range(row["records"])), set()
    new = update = reinsert = erase_found = erase_missing = 0
    peak = len(live)
    for s in samples:
        if not isinstance(s, dict) or not isinstance(s.get("type"), str) or s["type"] not in RATIOS or s.get("status") not in ("ok", "not_found") or not integer(s.get("key")) or not integer(s.get("rows")):
            raise ValueError("invalid operation sample")
        if not finite(s.get("latency_us")) or s["latency_us"] <= 0 or not finite(s.get("probe_seconds")):
            raise ValueError("invalid sample duration")
        counters(s.get("io")); counters(s.get("probe_io"))
        kind, key = s["type"], s["key"]
        missing = kind in ("get", "erase") and key not in live
        if s["status"] != ("not_found" if missing else "ok"):
            raise ValueError("sample status disagrees with independent key history")
        if kind == "get" and s["rows"] != (0 if missing else 1):
            raise ValueError("wrong Get result count")
        if kind == "range" and s["rows"] > row["range_width"] or kind in ("spatial", "contain") and s["rows"] > len(live) or kind == "knn" and s["rows"] != row["knn_k"]:
            raise ValueError("wrong query result count")
        if kind == "put":
            if key >= bound or s["rows"] != 0:
                raise ValueError("write outside the bounded domain")
            if key in live: update += 1
            else:
                new += 1; reinsert += key in erased
            live.add(key)
        if kind == "erase":
            if s["rows"] != 0: raise ValueError("erase returned rows")
            if missing: erase_missing += 1
            else: erase_found += 1; live.remove(key); erased.add(key)
        logical_write = int(kind == "put" or kind == "erase" and not missing)
        if s["io"]["logical_writes"] != logical_write:
            raise ValueError("logical write counter mismatch")
        if not with_probes:
            if s["probe_seconds"] != 0 or any(s["probe_io"].values()):
                raise ValueError("measurement trajectory contains a write probe")
        elif kind in ("put", "erase"):
            if s["probe_seconds"] <= 0: raise ValueError("write-after-read probe was omitted")
        elif s["probe_seconds"] != 0 or any(s["probe_io"].values()):
            raise ValueError("read sample unexpectedly contains a write probe")
        if any(s["probe_io"][k] != 0 for k in ("wal_bytes", "wal_appends", "logical_writes")):
            raise ValueError("probe performed a logical write")
        if not logical_write and any(s["io"][k] != 0 for k in ("wal_bytes", "wal_appends")):
            raise ValueError("read/missing erase appended a transaction")
        peak = max(peak, len(live))
        if not row["records"] // 2 <= len(live) <= bound:
            raise ValueError("key history exceeds its bounds")
    if (row["put_new"], row["put_update"], row["put_reinsert"], row["erase_found"], row["erase_missing"], row["final_records"], row["peak_records"]) != (new, update, reinsert, erase_found, erase_missing, len(live), peak):
        raise ValueError("mutation counts disagree with key history")
    if min(new, update, reinsert, erase_found, erase_missing) <= 0:
        raise ValueError("mutation semantics were not covered")
    if not with_probes or "measured_query_calls" in row:
        missed = sum(s["type"] == "get" and s["status"] == "not_found" for s in samples)
        missed += row["ops"] // 100 * 15 if with_probes else 0
        if row["uncounted_not_found_gets"] != missed:
            raise ValueError("uncounted not-found reads disagree with key history")
    for kind, proportion in RATIOS.items():
        group = [s for s in samples if s["type"] == kind]
        g = row["by_type"][kind]; n = row["ops"] // 100 * proportion
        if not isinstance(g, dict) or len(group) != n or any(not integer(g.get(k)) for k in ("count", "ok", "not_found", "rows")) or any(not finite(g.get(k)) for k in ("seconds", "p50_us", "p95_us", "p99_us")):
            raise ValueError("invalid per-type metrics")
        times = sorted(s["latency_us"] for s in group)
        if (g["count"], g["ok"], g["not_found"], g["rows"]) != (n, sum(s["status"] == "ok" for s in group), sum(s["status"] == "not_found" for s in group), sum(s["rows"] for s in group)):
            raise ValueError("per-type count mismatch")
        if not close(g["seconds"], sum(times) / 1e6) or any(not close(g[p], times[math.ceil(f * n) - 1]) for p, f in (("p50_us", .5), ("p95_us", .95), ("p99_us", .99))):
            raise ValueError("per-type latency cannot be reconstructed")
        counters(g.get("io"))
        if g["io"] != total(group, "io"): raise ValueError("per-type I/O cannot be reconstructed")
    if row["api_io"] != total(samples, "io") or row["probe_io"] != total(samples, "probe_io") or any(row["measurement_io"][k] != row["api_io"][k] + row["probe_io"][k] for k in COUNTERS):
        raise ValueError("measurement intervals do not add up")
    if not close(row["api_seconds"], sum(s["latency_us"] for s in samples) / 1e6) or not close(row["probe_seconds"], sum(s["probe_seconds"] for s in samples)):
        raise ValueError("measurement duration cannot be reconstructed")
    if row["api_seconds"] <= 0 or row["measured_wall_seconds"] < row["api_seconds"] + row["probe_seconds"] + row["oracle_seconds"] + row["snapshot_seconds"] or not close(row["throughput_api_ops_s"], row["ops"] / row["api_seconds"]) or not close(row["throughput_wall_ops_s"], row["ops"] / row["measured_wall_seconds"]):
        raise ValueError("invalid throughput or measurement boundary")
    if row["api_io"]["wal_bytes"] <= 0 or row["api_io"]["wal_appends"] <= 0 or row["api_io"]["logical_writes"] != row["ops"] // 100 * 15 + erase_found:
        raise ValueError("missing real transaction activity")
    if ((with_probes and row["probe_io"]["hits"] + row["probe_io"]["misses"] == 0)
        or row["checkpoint_io"]["fsyncs"] <= 0 or row["checkpoint_io"]["logical_writes"] != 0):
        raise ValueError("missing probes or checkpoint")
    if not 1 < row["initial_working_set_pages"] <= row["peak_physical_pages"]:
        raise ValueError("invalid page inventory bound")
    if row["regime"] == "resident":
        if row["pool_frames"] < row["peak_physical_pages"] or row["measurement_io"]["evictions"]:
            raise ValueError("resident capacity bound or zero-eviction evidence failed")
    elif row["regime"] == "evicting":
        if row["pool_frames"] >= row["initial_working_set_pages"] or min(row["api_io"]["evictions"], row["api_io"]["page_reads"]) <= 0:
            raise ValueError("missing API eviction evidence")
    else: raise ValueError("indeterminate cache regime")
    return row


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument("--build-dir", default="build-ninja")
    ap.add_argument("--output-dir")
    ap.add_argument("--records", type=int, default=512)
    ap.add_argument("--ops", type=int, default=300)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--frames", type=int, nargs="+", default=[12, 128])
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--warmup-ops", type=int, default=30)
    ap.add_argument("--range-width", type=int, default=16)
    ap.add_argument("--spatial-width", type=int, default=8)
    ap.add_argument("--knn-k", type=int, default=10)
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--trajectory", choices=("checked", "measure"), default="checked")
    args = ap.parse_args()
    if not (32 <= args.records <= 20000 and 100 <= args.ops <= 100000 and args.ops % 100 == 0 and 0 <= args.seed < 2**64 and
        0 <= args.warmup_ops <= 100000 and 0 < args.range_width <= args.records and 0 < args.spatial_width <= 1000000 and
        0 < args.knn_k <= args.records // 2 and args.repeats >= 3 and len(set(args.frames)) == len(args.frames) >= 2 and
        all(4 <= f <= 65536 for f in args.frames) and math.isfinite(args.timeout) and args.timeout > 0):
        ap.error("invalid limits; ops must be a multiple of 100; two frame counts and three repeats are required")
    build = (ROOT / args.build_dir).resolve(); build.relative_to(ROOT)
    exe = build / ("ltm_bench.exe" if sys.platform == "win32" else "ltm_bench")
    if not exe.is_file(): ap.error("build ltm_bench first")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    out = (ROOT / (args.output_dir or "report/bench/mixed/" + stamp)).resolve(); out.relative_to(ROOT)
    out.mkdir(parents=True, exist_ok=False)
    files = [ROOT / "CMakeLists.txt"] + [p for folder in ("src", "tests", "scripts", "tools", "demo") for p in (ROOT / folder).rglob("*") if p.suffix in (".cc", ".h", ".py")]
    manifest = dict(passed=False, started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), arguments=vars(args),
        exe=str(exe), binary_sha256=digest(exe), source_sha256={p.relative_to(ROOT).as_posix(): digest(p) for p in files}, runs=[])
    if (build / "CMakeCache.txt").is_file(): (out / "CMakeCache.txt").write_bytes((build / "CMakeCache.txt").read_bytes())
    rows, fingerprints = [], None
    def save(): (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    save()
    try:
        for frames in args.frames:
            for repeat in range(1, args.repeats + 1):
                run = out / ("frames%d_repeat%d" % (frames, repeat)); run.mkdir()
                database = run / "database"
                params = dict(seed=args.seed, records=args.records, ops=args.ops, warmup_ops=args.warmup_ops,
                    range_width=args.range_width, spatial_width=args.spatial_width, knn_k=args.knn_k, pool_frames=frames)
                flags = ["--workload=mixed", "--run-dir=" + str(database)] + ["--%s=%s" % (k, v) for k, v in {
                    "seed": args.seed, "records": args.records, "bench.ops": args.ops, "warmup-ops": args.warmup_ops,
                    "range-width": args.range_width, "spatial-width": args.spatial_width, "knn-k": args.knn_k,
                    "pool.pool_frames": frames, "trajectory": args.trajectory}.items()]
                pair = []
                for phase in ("run", "verify"):
                    command = [str(exe), *flags, "--phase=" + phase]
                    record = dict(command=command, phase=phase, frames=frames, repeat=repeat, passed=False)
                    manifest["runs"].append(record); save(); tick = time.perf_counter()
                    try:
                        process = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=args.timeout)
                    except subprocess.TimeoutExpired as error:
                        (run / (phase + ".stdout.txt")).write_bytes(error.stdout or b"")
                        (run / (phase + ".stderr.txt")).write_bytes(error.stderr or b"")
                        record["error"] = "timeout"; save(); raise ValueError("mixed process timed out") from error
                    (run / (phase + ".stdout.txt")).write_bytes(process.stdout); (run / (phase + ".stderr.txt")).write_bytes(process.stderr)
                    record.update(exit_code=process.returncode, seconds=time.perf_counter() - tick)
                    parsed = parse_result(process.stdout.decode("utf-8"), process.returncode, {**params, "phase": phase}, trajectory=args.trajectory)
                    if Path(parsed["run_dir"]).resolve() != database: raise ValueError("database path mismatch")
                    raw = process.stdout + process.stderr
                    if b"ThreadSanitizer" in raw or b"AddressSanitizer" in raw: raise ValueError("sanitizer report")
                    (run / (phase + ".result.json")).write_text(json.dumps(parsed, indent=2), encoding="utf-8")
                    pair.append(parsed); record["passed"] = True; save()
                hashes = tuple(pair[0][k] for k in HASHES)
                if hashes != tuple(pair[1][k] for k in HASHES) or fingerprints is not None and hashes != fingerprints:
                    raise ValueError("logical/input fingerprints differ across phases or runs")
                fingerprints = hashes
                for key in ("final_records", "peak_records", "record_bound"):
                    if pair[0][key] != pair[1][key]: raise ValueError("reopened inventory changed")
                (run / "database_files.json").write_text(json.dumps({p.name: dict(bytes=p.stat().st_size, sha256=digest(p)) for p in database.iterdir() if p.is_file()}, indent=2), encoding="utf-8")
                rows.append({**pair[0], "repeat": repeat})
                print("MIXED_OK: frames=%d repeat=%d regime=%s final=%d" % (frames, repeat, pair[0]["regime"], pair[0]["final_records"]), flush=True)
        if {r["regime"] for r in rows} != {"resident", "evicting"}: raise ValueError("both proven regimes are required")
        columns = ("trajectory", "schema_version", "measurement_scope", "frames", "repeat", "regime", "type", "count", "ok", "not_found", "rows", "seconds", "p50_us", "p95_us", "p99_us") + COUNTERS
        with (out / "results.csv").open("w", newline="", encoding="utf-8") as file:
            writer = csv.DictWriter(file, fieldnames=columns); writer.writeheader()
            for row in rows:
                for kind, group in row["by_type"].items():
                    writer.writerow(dict(trajectory=args.trajectory, schema_version=row["schema_version"], measurement_scope=row["measurement_scope"],
                        frames=row["pool_frames"], repeat=row["repeat"], regime=row["regime"], type=kind,
                        **{k: v for k, v in group.items() if k != "io"}, **group["io"]))
        summary = []
        for frames in args.frames:
            group = [r for r in rows if r["pool_frames"] == frames]
            summary.append(dict(frames=frames, regime=group[0]["regime"], repeats=len(group),
                api_throughput_median=statistics.median(r["throughput_api_ops_s"] for r in group),
                api_throughput_min=min(r["throughput_api_ops_s"] for r in group), api_throughput_max=max(r["throughput_api_ops_s"] for r in group),
                wall_throughput_median=statistics.median(r["throughput_wall_ops_s"] for r in group),
                page_reads_median=statistics.median(r["api_io"]["page_reads"] for r in group),
                trajectory=args.trajectory, schema_version=group[0]["schema_version"], measurement_scope=group[0]["measurement_scope"],
                note="Interleaved probes affect cache." if args.trajectory == "checked" else
                     "No extra write probes; inline full-record oracle and per-call snapshots remain; wall time includes them."))
        (out / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        manifest["passed"] = True
    except (OSError, ValueError, KeyError) as error:
        manifest["error"] = str(error); print("MIXED_ERROR: " + str(error), flush=True)
    finally:
        manifest["source_unchanged"] = all(digest(ROOT / name) == value for name, value in manifest["source_sha256"].items())
        manifest["binary_unchanged"] = digest(exe) == manifest["binary_sha256"]
        manifest["passed"] = manifest["passed"] and manifest["source_unchanged"] and manifest["binary_unchanged"]
        manifest["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat(); save()
    print("MIXED_VERDICT: " + ("passed" if manifest["passed"] else "failed"), flush=True)
    return 0 if manifest["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
