"""Negative gates against real mixed executable output, not fabricated metric fixtures."""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import mixed_bench

ROOT = mixed_bench.ROOT


class MixedProtocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build = os.environ.get("LTM_MIXED_TEST_BUILD", "build-ninja" if sys.platform == "win32" else "build-tsan-linux")
        cls.exe = ROOT / build / ("ltm_bench.exe" if sys.platform == "win32" else "ltm_bench")
        if not cls.exe.is_file(): raise RuntimeError("build ltm_bench before running mixed protocol checks")
        (ROOT / "report/bench").mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="mixed_protocol_", dir=ROOT / "report/bench")
        cls.root = Path(cls.temp.name).resolve(); cls.root.relative_to(ROOT)
        cls.flags = ["--workload=mixed", "--records=32", "--bench.ops=100", "--range-width=4", "--knn-k=4",
                     "--warmup-ops=0", "--pool.pool_frames=128", "--run-dir=" + str(cls.root / "database")]
        run = subprocess.run([str(cls.exe), *cls.flags], capture_output=True, timeout=120)
        cls.row = mixed_bench.parse_result(run.stdout.decode(), run.returncode, {"records": 32, "ops": 100, "phase": "run"})
        verify = subprocess.run([str(cls.exe), *cls.flags, "--phase=verify"], capture_output=True, timeout=120)
        cls.verify_row = mixed_bench.parse_result(verify.stdout.decode(), verify.returncode, {"phase": "verify"})
        cls.measure_flags = [s for s in cls.flags if not s.startswith("--run-dir=")] + [
            "--run-dir=" + str(cls.root / "measure_database"), "--trajectory=measure"]
        measured = subprocess.run([str(cls.exe), *cls.measure_flags], capture_output=True, timeout=120)
        cls.measure_row = mixed_bench.parse_result(measured.stdout.decode(), measured.returncode, {"phase": "run"}, trajectory="measure")
        reopened = subprocess.run([str(cls.exe), *cls.measure_flags, "--phase=verify"], capture_output=True, timeout=120)
        cls.measure_verify = mixed_bench.parse_result(reopened.stdout.decode(), reopened.returncode, {"phase": "verify"}, trajectory="measure")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def parse(self, row, code=0):
        return mixed_bench.parse_result("MIXED_RESULT " + json.dumps(row), code, {"ops": 100})

    def test_actual_run_and_fresh_reopen_have_same_records(self):
        self.assertEqual(tuple(self.row[k] for k in mixed_bench.HASHES), tuple(self.verify_row[k] for k in mixed_bench.HASHES))
        self.assertEqual(self.row["final_records"], self.verify_row["final_records"])
        self.assertEqual(self.row["by_type"]["put"]["count"], 15)

    def test_missing_fields_failed_exit_and_duplicate_results_are_rejected(self):
        for field in mixed_bench.FIELDS + mixed_bench.RUN_FIELDS:
            row = copy.deepcopy(self.row); del row[field]
            with self.subTest(field=field), self.assertRaises(ValueError): self.parse(row)
        with self.assertRaises(ValueError): self.parse(self.row, 1)
        for output in ("", "Open succeeded", "MIXED_RESULT {}", "MIXED_RESULT {}\nMIXED_RESULT {}"):
            with self.subTest(output=output), self.assertRaises(ValueError): mixed_bench.parse_result(output, 0, {})

    def test_invalid_counts_hashes_metrics_and_bounds_are_rejected(self):
        for change in ({"correct": False}, {"operation_hash": "bad"}, {"final_hash": str(2**64)}, {"records": True},
                       {"ops": 99}, {"final_records": 0}, {"peak_records": 1000}, {"i8_pairs_checked": 0},
                       {"api_seconds": 0}, {"oracle_seconds": float("nan")}, {"throughput_api_ops_s": 1},
                       {"measured_wall_seconds": 0}, {"regime": "indeterminate"}, {"i6_violations": 1}, {"put_update": 0}):
            with self.subTest(change=change), self.assertRaises(ValueError): self.parse({**self.row, **change})
        for path in (("by_type", "get", "count"), ("by_type", "put", "p99_us"), ("api_io", "wal_bytes"), ("measurement_io", "hits")):
            row = copy.deepcopy(self.row); target = row
            for key in path[:-1]: target = target[key]
            target[path[-1]] += 100
            with self.subTest(path=path), self.assertRaises(ValueError): self.parse(row)
        row = copy.deepcopy(self.row); del row["by_type"]["get"]
        with self.assertRaises(ValueError): self.parse(row)
        row = copy.deepcopy(self.verify_row); row["readonly_io"]["wal_bytes"] = 1
        with self.assertRaises(ValueError): self.parse(row)
        row = copy.deepcopy(self.row); row["samples"][0]["status"] = "not_found"
        with self.assertRaises(ValueError): self.parse(row)

    def test_existing_database_is_preserved_and_unknown_options_fail(self):
        db = self.root / "database"
        before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in db.iterdir()}
        run = subprocess.run([str(self.exe), *self.flags], capture_output=True, timeout=120)
        self.assertEqual(run.returncode, 2)
        self.assertNotIn(b"MIXED_RESULT", run.stdout)
        self.assertEqual(before, {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in db.iterdir()})
        for flag in ("--bench.ops=99", "--workload=unknown", "--unknown=1", "--records=0"):
            args = [s for s in self.flags if not s.startswith(flag.split("=")[0] + "=")]
            run = subprocess.run([str(self.exe), *args, flag], capture_output=True, timeout=120)
            self.assertEqual(run.returncode, 2, flag)
            self.assertNotIn(b"MIXED_RESULT", run.stdout)

    def test_real_timeout_cannot_generate_success_manifest(self):
        output = self.root / "timeout"
        run = subprocess.run([sys.executable, ROOT / "scripts/mixed_bench.py", "--output-dir", output,
            "--build-dir", self.exe.parent, "--records", "32", "--ops", "100", "--range-width", "4", "--knn-k", "4", "--timeout", "0.000001"],
            capture_output=True, timeout=120)
        self.assertEqual(run.returncode, 1)
        manifest = json.loads((output / "manifest.json").read_text())
        self.assertFalse(manifest["passed"])
        self.assertIn("timed out", manifest["error"])
        self.assertEqual(manifest["runs"][0]["error"], "timeout")

    def measure_parse(self, row):
        return mixed_bench.parse_result("MIXED_RESULT " + json.dumps(row), 0, {"ops": 100}, trajectory="measure")

    def test_both_trajectories_and_reopens_match_full_fingerprints(self):
        expected = tuple(self.row[k] for k in mixed_bench.HASHES)
        for row in (self.verify_row, self.measure_row, self.measure_verify):
            self.assertEqual(tuple(row[k] for k in mixed_bench.HASHES), expected)
            self.assertEqual(row["final_records"], self.row["final_records"])
        self.assertEqual(self.row["write_probe_calls"], 30)
        self.assertEqual(self.row["measured_read_requests"], 100)
        self.assertEqual(self.row["measured_query_calls"], 85 - self.row["by_type"]["get"]["not_found"])
        self.assertEqual(self.measure_row["write_probe_calls"], 0)
        self.assertEqual(self.measure_row["measured_read_requests"], 70)
        self.assertEqual(self.measure_row["measured_query_calls"], 70 - self.measure_row["by_type"]["get"]["not_found"])
        self.assertEqual(self.measure_row["probe_seconds"], 0)
        self.assertFalse(any(self.measure_row["probe_io"].values()))
        self.assertEqual(self.measure_row["measurement_io"], self.measure_row["api_io"])

    def test_schemas_cannot_be_mislabelled_or_cross_parsed(self):
        with self.assertRaises(ValueError): self.parse(self.measure_row)
        with self.assertRaises(ValueError): self.measure_parse(self.row)
        for key, value in (("schema_version", 1), ("trajectory", "checked"), ("write_probe_calls", 30),
                           ("measured_query_calls", 100), ("read_validation", "none"), ("stats_sampling", "none"),
                           ("measured_read_requests", 100), ("uncounted_not_found_gets", 100), ("query_counter_semantics", "none"),
                           ("measurement_scope", "api_calls_with_interleaved_probes")):
            row = copy.deepcopy(self.measure_row); row[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): self.measure_parse(row)
        for key in ("trajectory", "write_probe_calls", "measured_query_calls", "measured_read_requests", "uncounted_not_found_gets",
                    "read_validation", "stats_sampling", "query_counter_semantics"):
            row = copy.deepcopy(self.measure_row); del row[key]
            with self.subTest(key=key), self.assertRaises(ValueError): self.measure_parse(row)

    def test_hidden_probe_samples_and_counter_drift_are_rejected(self):
        for kind in mixed_bench.RATIOS:
            for field in ("probe_seconds", "probe_io"):
                row = copy.deepcopy(self.measure_row)
                sample = next(s for s in row["samples"] if s["type"] == kind)
                if field == "probe_seconds": sample[field] = .1
                else: sample[field]["hits"] = 1
                with self.subTest(kind=kind, field=field), self.assertRaises(ValueError): self.measure_parse(row)
        for field in ("api_io", "measurement_io", "probe_io"):
            row = copy.deepcopy(self.measure_row); row[field]["hits"] += 1
            with self.subTest(field=field), self.assertRaises(ValueError): self.measure_parse(row)

    def test_measure_protocol_keeps_existing_count_and_timing_gates(self):
        for key in mixed_bench.FIELDS + mixed_bench.RUN_FIELDS:
            row = copy.deepcopy(self.measure_row); del row[key]
            with self.subTest(key=key), self.assertRaises(ValueError): self.measure_parse(row)
        for change in ({"api_seconds": 0}, {"ops": 99}, {"final_hash": "bad"}, {"regime": "indeterminate"},
                       {"throughput_api_ops_s": 1}, {"i6_violations": 1}):
            with self.subTest(change=change), self.assertRaises(ValueError): self.measure_parse({**self.measure_row, **change})
        row = copy.deepcopy(self.measure_verify); row["readonly_io"]["logical_writes"] = 1
        with self.assertRaises(ValueError): self.measure_parse(row)

    def test_measure_cli_rejects_bad_trajectory_without_touching_database(self):
        database = self.root / "measure_database"
        before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in database.iterdir()}
        for flag in ("--trajectory=unknown", "--trajectory="):
            flags = [s for s in self.measure_flags if not s.startswith("--trajectory=")]
            run = subprocess.run([str(self.exe), *flags, flag], capture_output=True, timeout=120)
            self.assertEqual(run.returncode, 2)
            self.assertNotIn(b"MIXED_RESULT", run.stdout)
        self.assertEqual(before, {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in database.iterdir()})


if __name__ == "__main__": unittest.main()
