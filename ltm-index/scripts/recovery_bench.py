#!/usr/bin/env python3
"""Measure recovery after the same committed Hybrid input with explicit checkpoints."""
import argparse
import csv
import datetime
import hashlib
import json
import math
from pathlib import Path
import random
import shutil
import statistics
import struct
import subprocess
import sys
import time
import build_local

ROOT = Path(__file__).resolve().parents[1]
COUNTS = ('wal_bytes', 'records_scanned', 'pages_redone', 'records_redone', 'records_skipped',
          'txns_seen', 'txns_undone', 'checkpoint_lsn', 'redo_start_lsn', 'recover_us', 'storage_ready_us')

def sha(path):
    with path.open('rb') as f:
        h = hashlib.sha256()
        for block in iter(lambda: f.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()

def save(path, obj):
    path.write_text(json.dumps(obj, indent=2), encoding='utf-8')

def fingerprints():
    """Independent Python generation, including payload and all rectangle coordinates."""
    def row(key, generation):
        lo = (key % 23 * 4 + generation / 8, key // 23 * 4 + generation / 16)
        hi = (lo[0] + .75, lo[1] + .5)
        payload = f'{key}:recovery:{generation}'.encode('ascii')
        return (struct.pack('<QQ', key, 2) + b''.join(struct.pack('<dd', a, b) for a, b in zip(lo, hi))
                + struct.pack('<Q', len(payload)) + payload)
    def fnv(chunks):
        h = 14695981039346656037
        for chunk in chunks:
            for value in chunk: h = ((h ^ value) * 1099511628211) & ((1 << 64) - 1)
        return str(h)
    return (fnv(row(key, generation) for generation in (0, 1) for key in range(512)),
            fnv(row(key, 1) for key in range(512)))

def parse_result(stdout, code, stage, tail):
    if code != (9 if stage == 'prepare' else 0): raise ValueError('unexpected process exit')
    lines = [s[16:] for s in stdout.splitlines() if s.startswith('RECOVERY_RESULT ')]
    if len(lines) != 1: raise ValueError('expected exactly one recovery result')
    row = json.loads(lines[0])
    expected = dict(schema_version=1, stage=stage, engine='hybrid', records=512, updates=512,
                    pool_frames=24, page_size=4096, tail_updates=tail)
    if not isinstance(row, dict) or any(row.get(k) != v or type(row.get(k)) is not type(v) for k, v in expected.items()):
        raise ValueError('identity mismatch')
    input_hash, final_hash = fingerprints()
    if row.get('expected_hash') != final_hash: raise ValueError('independent expected hash mismatch')
    def counters(keys):
        if any(type(row.get(k)) is not int or row[k] < 0 for k in keys): raise ValueError('invalid counters')
    if stage == 'prepare':
        counters(('committed_puts', 'wal_file_bytes', 'checkpoint_before_bytes', 'checkpoint_kept_bytes', 'checkpoint_us', 'i6_violations', 'lock_violations', 'abrupt_exit_code'))
        if (row['committed_puts'] != 1024 or row['input_hash'] != input_hash or row['abrupt_exit_code'] != 9
                or row['i6_violations'] != 0 or row['lock_violations'] != 0
                or not 512 < row['checkpoint_kept_bytes'] < row['checkpoint_before_bytes']
                or row['wal_file_bytes'] < row['checkpoint_kept_bytes']):
            raise ValueError('invalid committed input or checkpoint evidence')
        if (tail == 0) != (row['wal_file_bytes'] == row['checkpoint_kept_bytes']):
            raise ValueError('checkpoint tail does not match WAL growth')
    elif stage == 'recover':
        counters(COUNTS)
        if (row.get('previous_clean_shutdown') is not False or row.get('storage_recovered') is not True
                or row.get('recovery_needed') is not True or row['txns_undone'] != 0
                or row['storage_ready_us'] < row['recover_us'] or row['checkpoint_lsn'] == 0
                or row['redo_start_lsn'] != row['checkpoint_lsn'] or row['records_scanned'] == 0):
            raise ValueError('recovery path not exercised')
        if tail == 0 and (row['pages_redone'] != 0 or row['records_redone'] != 0):
            raise ValueError('unexpected replay after final checkpoint')
        if tail > 0 and (row['pages_redone'] == 0 or row['records_redone'] == 0):
            raise ValueError('nonzero tail did not replay pages')
    elif stage == 'verify':
        counters(('point_checked', 'full_scan_rows', 'i8_pairs_checked', 'i6_violations', 'lock_violations'))
        if (row.get('correct') is not True or row['dataset_hash'] != final_hash
                or any(row[k] != 512 for k in ('point_checked', 'full_scan_rows', 'i8_pairs_checked'))
                or row['i6_violations'] != 0 or row['lock_violations'] != 0):
            raise ValueError('incomplete or incorrect restored records')
    else: raise ValueError('unsupported stage')
    return row

def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument('--build-dir', default='build-recovery')
    ap.add_argument('--output-dir')
    ap.add_argument('--build-only', action='store_true')
    ap.add_argument('--skip-build', action='store_true')
    ap.add_argument('--asan', action='store_true', help='correctness checks only; not performance comparisons')
    ap.add_argument('--tails', type=int, nargs='+', default=[0, 16, 64, 256, 512])
    ap.add_argument('--repeats', type=int, default=3)
    ap.add_argument('--timeout', type=float, default=180)
    args = ap.parse_args()
    if not 1 <= args.repeats <= 10 or len(set(args.tails)) != len(args.tails) or any(not 0 <= t <= 512 for t in args.tails):
        ap.error('tails must be unique in 0..512 and repeats must be in 1..10')
    if not math.isfinite(args.timeout) or args.timeout <= 0: ap.error('invalid timeout')
    build = (ROOT / args.build_dir).resolve(); build.relative_to(ROOT)
    if build == ROOT or build.name in ('build-ninja', 'build-asan', 'build-dbg', 'build-fi', 'build-tsan-linux'):
        ap.error('use a separate recovery build directory')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    out = (ROOT / (args.output_dir or 'report/recovery/' + stamp)).resolve(); out.relative_to(ROOT)
    if not out.is_relative_to(ROOT / 'report'): ap.error('output must be inside report')
    out.mkdir(parents=True, exist_ok=False)
    manifest = dict(passed=False, arguments=vars(args), started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    configuration='asan correctness only' if args.asan else 'ordinary RelWithDebInfo', runs=[],
                    method='fixed 512 inserts and 512 updates; one explicit checkpoint; abrupt exit 9 after final committed Put; three separate processes per case; warm OS cache')
    def persist(): save(out / 'manifest.json', manifest)
    def run(label, command, expected_code):
        tick = time.perf_counter()
        try:
            result = subprocess.run(list(map(str, command)), cwd=ROOT, env=env, capture_output=True, timeout=args.timeout)
        except subprocess.TimeoutExpired as exc:
            (out / (label + '.stdout.txt')).parent.mkdir(parents=True, exist_ok=True)
            (out / (label + '.stdout.txt')).write_bytes(exc.stdout or b'')
            (out / (label + '.stderr.txt')).write_bytes(exc.stderr or b'')
            raise ValueError(label + ' timed out') from exc
        (out / (label + '.stdout.txt')).parent.mkdir(parents=True, exist_ok=True)
        (out / (label + '.stdout.txt')).write_bytes(result.stdout)
        (out / (label + '.stderr.txt')).write_bytes(result.stderr)
        manifest['runs'].append(dict(label=label, command=list(map(str, command)), exit_code=result.returncode,
                                     wall_seconds=time.perf_counter() - tick, exit_expected=result.returncode == expected_code))
        persist()
        if result.returncode != expected_code: raise ValueError(label + ' failed; inspect stderr')
        return result.stdout.decode('utf-8')
    persist()
    try:
        env, cmake, ninja = build_local.build_env()
        old_files = [p for name in ('build-ninja', 'build-asan', 'build-dbg', 'build-fi', 'build-tsan-linux')
                     for sub in ('', 'RelWithDebInfo', 'Release', 'Debug') for p in (ROOT / name / sub).glob('*')
                     if p.suffix in ('.exe', '.lib', '.dll', '.a') and p.is_file()]
        manifest['historical_binary_sha256'] = {str(p.relative_to(ROOT)): sha(p) for p in old_files}
        files = [ROOT / 'CMakeLists.txt', ROOT / 'scripts/build_local.py', ROOT / 'scripts/msvc_compile.py', Path(__file__)]
        files += sorted((ROOT / 'src').rglob('*.h')) + sorted((ROOT / 'src').rglob('*.cc'))
        files += sorted((ROOT / 'tools/recovery_bench').glob('*'))
        manifest['source_sha256'] = {str(p.relative_to(ROOT)): sha(p) for p in files if p.is_file()}
        if not args.skip_build:
            run('configure', [cmake, '-S', ROOT / 'tools/recovery_bench', '-B', build, '-G', 'Ninja',
                '-DCMAKE_MAKE_PROGRAM=' + ninja, '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DPython3_EXECUTABLE=' + sys.executable,
                '-DLTM_ENABLE_ASAN=' + ('ON' if args.asan else 'OFF'), '-DLTM_ENABLE_TSAN=OFF', '-DLTM_WERROR=ON'], 0)
            run('build', [cmake, '--build', build, '--target', 'ltm_recovery_bench', '-j', '4'], 0)
        exe = build / 'ltm_recovery_bench.exe'
        if not exe.is_file(): raise ValueError('build the recovery executable first')
        cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
        if f'LTM_ENABLE_ASAN:BOOL={"ON" if args.asan else "OFF"}' not in cache or 'LTM_ENABLE_TSAN:BOOL=OFF' not in cache:
            raise ValueError('build sanitizer configuration mismatch')
        (out / 'CMakeCache.txt').write_text(cache, encoding='utf-8')
        manifest.update(exe=str(exe), binary_sha256=sha(exe))
        results = []
        if not args.build_only:
            order = [(tail, repeat) for repeat in range(1, args.repeats + 1) for tail in args.tails]
            random.Random(42).shuffle(order); manifest['case_order'] = order; persist()
            for tail, repeat in order:
                case = out / f'tail{tail}_repeat{repeat}'; case.mkdir()
                print(f'RECOVERY_CASE tail={tail} repeat={repeat}', flush=True)
                identity = ['--tail-updates=' + str(tail), '--run-dir=' + str(case / 'database')]
                prepared = parse_result(run(case.name + '/prepare', [exe, '--stage=prepare'] + identity, 9), 9, 'prepare', tail)
                save(case / 'prepare.result.json', prepared)
                snapshot = case / 'crash_snapshot'; shutil.copytree(case / 'database', snapshot)
                snapshots = {p.name: dict(bytes=p.stat().st_size, sha256=sha(p)) for p in snapshot.iterdir() if p.is_file()}
                if snapshots['records.wal']['bytes'] != prepared['wal_file_bytes']: raise ValueError('crash WAL size mismatch')
                recovered = parse_result(run(case.name + '/recover', [exe, '--stage=recover'] + identity, 0), 0, 'recover', tail)
                save(case / 'recover.result.json', recovered)
                if recovered['wal_bytes'] != prepared['wal_file_bytes']: raise ValueError('recovery did not scan crash WAL')
                if recovered['txns_seen'] != tail: raise ValueError('unexpected checkpoint tail transaction count')
                verified = parse_result(run(case.name + '/verify', [exe, '--stage=verify'] + identity, 0), 0, 'verify', tail)
                save(case / 'verify.result.json', verified)
                if not all(sha(snapshot / name) == obj['sha256'] for name, obj in snapshots.items()): raise ValueError('crash snapshot changed')
                row = dict(tail_updates=tail, repeat=repeat, input_hash=prepared['input_hash'], dataset_hash=verified['dataset_hash'],
                           checkpoint_us=prepared['checkpoint_us'], checkpoint_before_bytes=prepared['checkpoint_before_bytes'],
                           checkpoint_kept_bytes=prepared['checkpoint_kept_bytes'], **{k: recovered[k] for k in COUNTS})
                results.append(row); manifest.setdefault('cases', []).append(dict(case=case.name, passed=True, snapshot=snapshots)); persist()
            summary = []
            for tail in sorted(args.tails):
                group = [r for r in results if r['tail_updates'] == tail]
                for key in ('wal_bytes', 'records_scanned', 'input_hash', 'dataset_hash', 'txns_seen'):
                    if len({r[key] for r in group}) != 1: raise ValueError('repetition identity changed')
                summary.append(dict(tail_updates=tail, repetitions=len(group), wal_bytes=group[0]['wal_bytes'],
                    records_scanned=group[0]['records_scanned'], recover_us_min=min(r['recover_us'] for r in group),
                    recover_us_median=statistics.median(r['recover_us'] for r in group), recover_us_max=max(r['recover_us'] for r in group),
                    storage_ready_us_median=statistics.median(r['storage_ready_us'] for r in group),
                    checkpoint_us_median=statistics.median(r['checkpoint_us'] for r in group),
                    records_redone_median=statistics.median(r['records_redone'] for r in group), pages_redone_median=statistics.median(r['pages_redone'] for r in group)))
            if len({r['input_hash'] for r in results}) != 1 or len({r['dataset_hash'] for r in results}) != 1:
                raise ValueError('checkpoint groups changed workload or final state')
            save(out / 'results.json', results); save(out / 'summary.json', summary)
            with (out / 'results.csv').open('w', newline='', encoding='utf-8') as f:
                writer = csv.DictWriter(f, fieldnames=list(results[0])); writer.writeheader(); writer.writerows(results)
        manifest['source_unchanged'] = all(sha(ROOT / name) == digest for name, digest in manifest['source_sha256'].items())
        manifest['historical_binaries_unchanged'] = all(sha(ROOT / name) == digest for name, digest in manifest['historical_binary_sha256'].items())
        manifest['binary_unchanged'] = sha(exe) == manifest['binary_sha256']
        if not all(manifest[k] for k in ('source_unchanged', 'historical_binaries_unchanged', 'binary_unchanged')): raise ValueError('source or binary changed during experiment')
        manifest.update(passed=True, successful_cases=len(results), independent_input_hash=fingerprints()[0], independent_final_hash=fingerprints()[1])
    except (ValueError, OSError) as exc:
        manifest['error'] = str(exc); print('RECOVERY_ERROR: ' + str(exc), file=sys.stderr)
    manifest['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat(); persist()
    print('RECOVERY_VERDICT: ' + ('passed' if manifest['passed'] else 'failed'), flush=True)
    print('RECOVERY_REPORT: ' + str(out), flush=True)
    return 0 if manifest['passed'] else 1

if __name__ == '__main__': sys.exit(main())
