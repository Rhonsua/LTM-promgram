#!/usr/bin/env python3
"""Guard recovery evidence against false positive process and timing results."""
import copy
import json
import unittest
import recovery_bench as bench

class EvidenceProtocol(unittest.TestCase):
    def setUp(self):
        self.row = dict(schema_version=1, stage='recover', engine='hybrid', records=512, updates=512,
            pool_frames=24, page_size=4096, tail_updates=16, expected_hash=bench.fingerprints()[1],
            previous_clean_shutdown=False, storage_recovered=True, recovery_needed=True,
            recover_us=100, storage_ready_us=110, wal_bytes=10000, records_scanned=80, pages_redone=4,
            records_redone=8, records_skipped=72, txns_seen=16, txns_undone=0, checkpoint_lsn=123,
            redo_start_lsn=123)
    def parse(self, row=None, code=0, tail=16):
        return bench.parse_result('RECOVERY_RESULT ' + json.dumps(self.row if row is None else row), code, 'recover', tail)
    def reject(self, **changes):
        row = copy.deepcopy(self.row); row.update(changes)
        with self.assertRaises(ValueError): self.parse(row)
    def test_valid_replay(self): self.assertEqual(self.parse()['records_redone'], 8)
    def test_nonzero_exit(self):
        with self.assertRaises(ValueError): self.parse(code=1)
    def test_clean_shutdown_is_not_crash_evidence(self): self.reject(previous_clean_shutdown=True)
    def test_empty_replay_is_not_tail_evidence(self): self.reject(records_redone=0, pages_redone=0)
    def test_parameters_must_match(self): self.reject(pool_frames=256)
    def test_boolean_is_not_counter(self): self.reject(recover_us=True)
    def test_missing_counter(self):
        row = copy.deepcopy(self.row); del row['records_scanned']
        with self.assertRaises(ValueError): self.parse(row)
    def test_inconsistent_timing(self): self.reject(recover_us=120, storage_ready_us=110)
    def test_negative_timing(self): self.reject(recover_us=-1)
    def test_wrong_expected_record_fingerprint(self): self.reject(expected_hash='1')
    def test_missing_checkpoint(self): self.reject(checkpoint_lsn=0, redo_start_lsn=0)
    def test_wrong_recovery_start(self): self.reject(redo_start_lsn=122)
    def test_uncommitted_work_is_a_different_experiment(self): self.reject(txns_undone=1)
    def test_duplicate_marker(self):
        line = 'RECOVERY_RESULT ' + json.dumps(self.row)
        with self.assertRaises(ValueError): bench.parse_result(line + '\n' + line, 0, 'recover', 16)
    def test_no_marker(self):
        with self.assertRaises(ValueError): bench.parse_result('completed successfully', 0, 'recover', 16)
    def test_zero_tail_control(self):
        row = copy.deepcopy(self.row); row.update(tail_updates=0, pages_redone=0, records_redone=0, txns_seen=0)
        self.assertEqual(self.parse(row, tail=0)['records_redone'], 0)
    def test_zero_tail_cannot_hide_replay(self):
        row = copy.deepcopy(self.row); row.update(tail_updates=0)
        with self.assertRaises(ValueError): self.parse(row, tail=0)

if __name__ == '__main__': unittest.main()
