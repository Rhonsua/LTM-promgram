"""Regression checks for false-success paths in the benchmark result protocol."""
import copy
import json
import unittest
import bench


class ResultContract(unittest.TestCase):
    def setUp(self):
        self.row = dict.fromkeys(bench.COUNTERS, 0)
        self.row.update(dict.fromkeys(bench.FLOATS, 1.0))
        self.row.update(schema_version=1, engine="bplus", threads=1, workload="point", seed=42,
                        records=32, ops=10, warmup_ops=1, range_width=4, pool_frames=16,
                        page_size=4096, working_set_pages=3, regime="resident", dataset_hash="123",
                        query_hash="456", correct=True, order_capacity=32, heat="none", policy="clock",
                        config_dump="pool.pool_frames=16", run_dir="test", hits=10, result_rows=10,
                        throughput_ops_s=10.0)

    def parse(self, row, code=0):
        return bench.parse_result("BENCH_RESULT " + json.dumps(row), code, {"ops": 10, "seed": 42})

    def test_complete_valid_record(self):
        self.assertEqual(self.parse(self.row)["ops"], 10)

    def test_old_skeleton_duplicate_or_missing_record_fails(self):
        for output in ("Open took 1ms", "not implemented yet", "", "BENCH_RESULT {}", "BENCH_RESULT {}\nBENCH_RESULT {}"):
            with self.subTest(output=output), self.assertRaises(ValueError):
                bench.parse_result(output, 0, {})

    def test_nonzero_exit_cannot_be_rescued_by_valid_metrics(self):
        with self.assertRaises(ValueError): self.parse(self.row, 1)

    def test_every_required_field_is_required(self):
        for field in bench.FIELDS:
            row = copy.deepcopy(self.row); del row[field]
            with self.subTest(field=field), self.assertRaises(ValueError): self.parse(row)

    def test_corrupt_metrics_and_mismatched_inputs_fail(self):
        changes = [{"ops": 0}, {"seed": 43}, {"correct": False}, {"page_reads": 1},
                   {"p99_us": float("nan")}, {"p50_us": 2}, {"throughput_ops_s": 99},
                   {"hits": True}, {"i6_violations": 1}, {"regime": "evicting"},
                   {"working_set_pages": 0}, {"wal_bytes": 1}, {"result_rows": 9},
                   {"config_dump": None}, {"heat": "injected"}, {"hit_rate": .5},
                   {"measurement_seconds": 0}, {"measured_wall_seconds": .5}]
        for change in changes:
            row = {**self.row, **change}
            with self.subTest(change=change), self.assertRaises(ValueError): self.parse(row)

    def hybrid(self):
        return {**self.row, "schema_version": 2, "engine": "hybrid", "workload": "knn",
                "heat": "enabled", "policy": "migration", "spatial_width": 8, "knn_k": 2,
                "mbr_dim": 2, "split_policy": "quadratic", "oracle_seconds": .1,
                "bplus_pages": 3, "rtree_pages": 4, "active_pages": 5,
                "working_set_pages": 8, "i8_pairs_checked": 32, "result_rows": 20}

    def test_hybrid_fields_and_policy_are_checked(self):
        row = self.hybrid(); self.assertEqual(self.parse(row)["engine"], "hybrid")
        for field in bench.HYBRID_FIELDS:
            missing = {k: v for k, v in row.items() if k != field}
            with self.subTest(field=field), self.assertRaises(ValueError): self.parse(missing)
        for change in ({"knn_k": True}, {"knn_k": 33}, {"result_rows": 19},
                       {"heat": "none"}, {"policy": "clock"}, {"i8_pairs_checked": 31},
                       {"rtree_pages": 5}, {"active_pages": 4}, {"oracle_seconds": float("nan")},
                       {"mbr_dim": 3}, {"schema_version": 1}, {"workload": "mix"},
                       {"split_policy": "linear"}, {"spatial_width": 0}):
            with self.subTest(change=change), self.assertRaises(ValueError): self.parse({**row, **change})

    def test_hybrid_eviction_requires_active_tree_to_exceed_pool(self):
        row = {**self.hybrid(), "pool_frames": 4, "regime": "evicting", "misses": 10,
               "page_reads": 10, "evictions": 10, "hit_rate": .5}
        self.assertEqual(self.parse(row)["regime"], "evicting")
        with self.assertRaises(ValueError): self.parse({**row, "pool_frames": 6})

    def test_empty_spatial_results_are_valid(self):
        self.assertEqual(self.parse({**self.hybrid(), "workload": "spatial", "result_rows": 0})["result_rows"], 0)
        with self.assertRaises(ValueError): self.parse({**self.hybrid(), "workload": "contain", "result_rows": 321})


if __name__ == "__main__": unittest.main()
