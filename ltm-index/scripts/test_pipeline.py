"""Reject false-success outputs from the functional pipeline."""
import json
import unittest
import run_pipeline


class PipelineContract(unittest.TestCase):
    def row(self, stage="load"):
        result = dict.fromkeys(run_pipeline.INTS, 0)
        result.update(schema_version=1, stage=stage, correct=True, records=512, pool_frames=12,
                      seed=42, point_checked=512, missing_checked=1, full_scan_rows=512,
                      range_rows=65, spatial_rows=9, contain_rows=4, empty_rows=0, knn_rows=10,
                      i8_pairs_checked=512, working_set_pages=55, logical_writes=512 if stage == "load" else 0,
                      page_reads=100, page_writes=100 if stage == "load" else 0,
                      wal_bytes=1000 if stage == "load" else 0, evictions=88, dataset_hash="123")
        return result

    def parse(self, row, stage="load", code=0):
        return run_pipeline.parse_result("PIPELINE_RESULT " + json.dumps(row), code, stage)

    def test_load_and_fresh_process_read(self):
        self.assertEqual(self.parse(self.row())["logical_writes"], 512)
        self.assertEqual(self.parse(self.row("verify"), "verify")["logical_writes"], 0)

    def test_nonzero_missing_or_duplicate_result_fails(self):
        with self.assertRaises(ValueError): self.parse(self.row(), code=1)
        for text in ("", "completed", "PIPELINE_RESULT {}", "PIPELINE_RESULT {}\nPIPELINE_RESULT {}"):
            with self.subTest(text=text), self.assertRaises(ValueError): run_pipeline.parse_result(text, 0, "load")

    def test_every_counter_and_identity_is_required(self):
        row = self.row()
        for key in run_pipeline.INTS + ("stage", "correct", "dataset_hash"):
            with self.subTest(key=key), self.assertRaises(ValueError): self.parse({k: v for k, v in row.items() if k != key})

    def test_fake_success_and_absent_real_io_fail(self):
        for change in ({"correct": False}, {"correct": 1}, {"records": True}, {"point_checked": 511},
                       {"i8_pairs_checked": 511}, {"stage": "verify"}, {"i6_violations": 1},
                       {"page_reads": 0}, {"page_writes": 0}, {"wal_bytes": 0}, {"evictions": 0},
                       {"working_set_pages": 12}, {"contain_rows": 9}, {"dataset_hash": "bad"}, {"knn_rows": 9}):
            with self.subTest(change=change), self.assertRaises(ValueError): self.parse({**self.row(), **change})

    def test_verify_must_not_repopulate_the_database(self):
        with self.assertRaises(ValueError): self.parse({**self.row("verify"), "logical_writes": 512}, "verify")


if __name__ == "__main__": unittest.main()
