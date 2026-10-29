"""Actual executable protocol/CLI checks; no canned successful output fixtures."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from knn_bench import PREFIX, digest, parse_result


@unittest.skipUnless(os.environ.get('LTM_KNN_BENCH'), 'set LTM_KNN_BENCH to run integration tests')
class KnnProtocolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = os.environ['LTM_KNN_BENCH']
        parent = Path(os.environ.get('LTM_KNN_TEST_DIR', tempfile.gettempdir()))
        parent.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='knn-test-', dir=parent)
        cls.directory = Path(cls.temp.name) / 'database'
        cls.settings = dict(records=512, ops=16, warmup=16, frames=128, readers=3, k=8, seed=42, regime='resident')
        cls.invoke('prepare')
        cls.raw = cls.invoke('measure').stdout.decode('utf-8', errors='replace')
        cls.result = parse_result(cls.raw, dict(cls.settings, stage='measure'))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @classmethod
    def command(cls, stage):
        return [cls.exe, '--run-dir=' + str(cls.directory)] + ['--%s=%s' % item for item in dict(cls.settings, stage=stage).items()]

    @classmethod
    def invoke(cls, stage):
        result = subprocess.run(cls.command(stage), capture_output=True, timeout=120)
        if result.returncode:
            raise AssertionError(result.stderr.decode('utf-8', errors='replace') + result.stdout.decode('utf-8', errors='replace'))
        return result

    def reject(self, **changes):
        row = copy.deepcopy(self.result); row.update(changes)
        with self.assertRaises(ValueError):
            parse_result(PREFIX + json.dumps(row), dict(self.settings, stage='measure'))

    def test_actual_nondivisible_fixed_work_and_reopen(self):
        self.assertEqual(self.result['worker_counts'], [6, 5, 5])
        reopened = parse_result(self.invoke('verify').stdout.decode('utf-8', errors='replace'), dict(self.settings, stage='verify'))
        self.assertEqual(reopened['answer_hash'], self.result['answer_hash'])

    def test_reader_counts_do_not_multiply_work(self):
        for counts in ([16, 16, 16], [5, 5, 6], [6, 5], [True, 5, 5]):
            with self.subTest(counts=counts): self.reject(worker_counts=counts)
        self.reject(query_calls=48); self.reject(validated_queries=15)

    def test_schema_scope_and_identity(self):
        self.reject(schema_version=2); self.reject(measurement_scope='inline')
        self.reject(readers=1); self.reject(k=9); self.reject(correct=False)

    def test_writes_and_residency_rejected(self):
        self.reject(page_writes=1); self.reject(wal_appends=1)
        self.reject(page_reads=1); self.reject(evictions=1)
        self.reject(full_pages=129)

    def test_invalid_timing_and_types(self):
        self.reject(wall_ns=0); self.reject(p95_ns=0); self.reject(api_sum_ns=2**63)
        self.reject(p50_ns=True); self.reject(page_reads=0.0); self.reject(answer_hash='-1')

    def test_duplicate_extra_and_missing_output(self):
        for raw in ('', self.raw + self.raw, PREFIX + json.dumps(self.result)[:-1] + ',"ops":16}',
                    PREFIX + json.dumps(dict(self.result, invented=1))):
            with self.subTest(raw=raw[:50]), self.assertRaises(ValueError):
                parse_result(raw, dict(self.settings, stage='measure'))

    def test_nonmeasurement_stage_cannot_claim_timings(self):
        row = json.loads(self.invoke('verify').stdout.decode('utf-8', errors='replace').split(PREFIX)[1])
        row['wall_ns'] = 1
        with self.assertRaises(ValueError): parse_result(PREFIX + json.dumps(row), dict(self.settings, stage='verify'))

    def test_invalid_cli_and_overwrite_preserve_database(self):
        files = [self.directory / name for name in ('pages.data', 'records.wal')]
        before = [digest(path) for path in files]
        bad = ['--readers=0', '--k=0', '--ops=99999999999', '--unknown=1', '--seed=-1', '--regime=other']
        for extra in bad:
            result = subprocess.run(self.command('prepare') + [extra], capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 2)
        result = subprocess.run(self.command('prepare'), capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 1)
        self.assertIn(b'refusing existing database', result.stderr)
        self.assertEqual(before, [digest(path) for path in files])

    def test_missing_database_is_not_created(self):
        missing = Path(self.temp.name) / 'absent'
        command = [part.replace(str(self.directory), str(missing)) for part in self.command('verify')]
        result = subprocess.run(command, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 1); self.assertFalse(missing.exists())

    def test_actual_process_timeout(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            subprocess.run(self.command('verify'), capture_output=True, timeout=0.000001)


if __name__ == '__main__': unittest.main()
