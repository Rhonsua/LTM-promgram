"""Build and audit fixed-work disk-backed KNN reader measurements.

Default: 54 ordinary measurements and 54 independent-process reopens.
Sanitizer profiles require --quick; their timings are never performance evidence.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
PREFIX = 'KNN_RESULT '


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def parse_result(raw, expected):
    lines = [line[len(PREFIX):] for line in raw.splitlines() if line.startswith(PREFIX)]
    if len(lines) != 1:
        raise ValueError('expected exactly one KNN_RESULT')
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate result field')
            result[key] = value
        return result
    row = json.loads(lines[0], object_pairs_hook=pairs)
    fields = ('schema_version stage regime measurement_scope records ops warmup frames readers k seed '
              'full_pages active_pages dataset_hash query_hash answer_hash validated_queries wall_ns '
              'api_sum_ns p50_ns p95_ns p99_ns page_reads evictions page_writes wal_appends query_calls '
              'worker_counts correct').split()
    if set(row) != set(fields):
        raise ValueError('unexpected or missing result field')
    numeric = set(fields) - {'stage', 'regime', 'measurement_scope', 'dataset_hash', 'query_hash',
                             'answer_hash', 'worker_counts', 'correct'}
    if any(type(row[key]) is not int or row[key] < 0 for key in numeric):
        raise ValueError('invalid nonnegative integer')
    if row['schema_version'] != 1 or row['correct'] is not True or row['measurement_scope'] != 'fixed_total_queries_deferred_validation':
        raise ValueError('wrong schema or measurement scope')
    for key, value in expected.items():
        if row.get(key) != value:
            raise ValueError('input mismatch: ' + key)
    for key in ('dataset_hash', 'query_hash', 'answer_hash'):
        if type(row[key]) is not str or not row[key].isdigit() or int(row[key]) >= 2**64:
            raise ValueError('invalid fingerprint')
    if (row['regime'] not in ('resident', 'evicting') or row['stage'] not in ('prepare', 'measure', 'verify') or
            not 64 <= row['records'] <= 8192 or not 8 <= row['ops'] <= 8192 or
            not 1 <= row['warmup'] <= row['ops'] or not 1 <= row['readers'] <= min(8, row['ops']) or
            not 12 <= row['frames'] <= 65536 or not 1 <= row['k'] <= row['records'] or
            row['ops'] * row['k'] > 524288 or row['full_pages'] < row['active_pages'] or row['active_pages'] <= 1):
        raise ValueError('invalid workload or structure')
    if row['regime'] == 'resident':
        if row['full_pages'] > row['frames']:
            raise ValueError('resident page bound failed')
    elif row['active_pages'] <= row['frames']:
        raise ValueError('evicting page bound failed')
    counts = row['worker_counts']
    if type(counts) is not list or len(counts) != row['readers'] or any(type(n) is not int or n < 0 for n in counts):
        raise ValueError('invalid worker counts')
    if row['stage'] == 'measure':
        expected_counts = [len(range(i, row['ops'], row['readers'])) for i in range(row['readers'])]
        if counts != expected_counts or row['query_calls'] != row['ops'] or row['validated_queries'] != row['ops']:
            raise ValueError('fixed total query accounting failed')
        if (not 0 < row['p50_ns'] <= row['p95_ns'] <= row['p99_ns'] <= row['api_sum_ns'] or row['wall_ns'] <= 0 or
                row['api_sum_ns'] > row['readers'] * row['wall_ns'] or row['page_writes'] != 0 or row['wal_appends'] != 0):
            raise ValueError('invalid timing or writes in read interval')
        if row['regime'] == 'resident':
            if row['page_reads'] or row['evictions']:
                raise ValueError('resident measurement performed reads/evictions')
        elif not row['page_reads'] or not row['evictions']:
            raise ValueError('evicting measurement did not evict/read')
    else:
        zero = ('wall_ns', 'api_sum_ns', 'p50_ns', 'p95_ns', 'p99_ns', 'page_reads', 'evictions', 'page_writes', 'wal_appends', 'query_calls')
        if any(row[key] != 0 for key in zero) or any(counts) or row['validated_queries'] != (row['ops'] if row['stage'] == 'verify' else 0):
            raise ValueError('non-measured stage contains timing/counter data')
    return row


def source_files():
    files = [ROOT / 'CMakeLists.txt']
    for folder in ('src', 'scripts', 'tools/knn_bench'):
        files += [p for p in (ROOT / folder).rglob('*') if p.suffix in ('.cc', '.h', '.py', '.txt') and '__pycache__' not in p.parts]
    return sorted(set(files))


def audit(output, require_current=True):
    output = Path(output).resolve()
    manifest = json.loads((output / 'manifest.json').read_text(encoding='utf-8'))
    if not manifest.get('completed'):
        raise ValueError('incomplete run')
    for relative, sha in manifest['sources'].items():
        if digest(output / 'source' / relative) != sha or (require_current and digest(ROOT / relative) != sha):
            raise ValueError('source mismatch: ' + relative)
    if digest(manifest['executable']) != manifest['executable_sha256']:
        raise ValueError('executable changed')
    parsed = []
    for call in manifest['commands']:
        for suffix in ('stdout', 'stderr'):
            path = output / (call['name'] + '.' + suffix + '.txt')
            if digest(path) != call[suffix + '_sha256']:
                raise ValueError('raw output changed')
        if call['exit_code'] != call.get('expected_exit', 0):
            raise ValueError('failed command')
        if 'expected' in call:
            row = parse_result((output / (call['name'] + '.stdout.txt')).read_text(encoding='utf-8', errors='replace'), call['expected'])
            if row != call['result']:
                raise ValueError('raw result differs from manifest')
            parsed.append((call['name'], row))
    measurements = [(name, row) for name, row in parsed if row['stage'] == 'measure']
    if len(measurements) != manifest['expected_measurements'] or len(parsed) != 3 * len(measurements):
        raise ValueError('incomplete measurement/reopen matrix')
    by_name = dict(parsed)
    inputs = set(); answers = {}; groups = {}
    for name, row in measurements:
        stem = name.removesuffix('_measure')
        prepare, verify = by_name[stem + '_prepare'], by_name[stem + '_verify']
        identity = ('records', 'ops', 'warmup', 'frames', 'readers', 'k', 'seed', 'regime', 'dataset_hash', 'query_hash', 'full_pages', 'active_pages')
        if any(prepare[key] != row[key] or verify[key] != row[key] for key in identity) or verify['answer_hash'] != row['answer_hash']:
            raise ValueError('fresh-process persistence/identity mismatch')
        inputs.add((row['dataset_hash'], row['query_hash']))
        answers.setdefault(row['k'], set()).add(row['answer_hash'])
        key = (row['regime'], row['frames'], row['k'], row['readers'])
        groups.setdefault(key, []).append(row)
    if len(inputs) != 1 or any(len(v) != 1 for v in answers.values()):
        raise ValueError('different fixed input or answers across reader/frame/repetition settings')
    expected_groups = {(regime, frames, k, readers) for regime, frames in manifest['regimes']
                       for k in manifest['ks'] for readers in manifest['readers']}
    if set(groups) != expected_groups or any(len(rows) != manifest['repetitions'] for rows in groups.values()):
        raise ValueError('missing/duplicate matrix cell')
    summary = []
    for key, rows in sorted(groups.items()):
        entry = dict(zip(('regime', 'frames', 'k', 'readers'), key))
        entry['repetitions'] = len(rows)
        for metric in ('wall_qps', 'p50_us', 'p95_us', 'p99_us', 'page_reads', 'evictions'):
            values = [r['ops'] * 1e9 / r['wall_ns'] if metric == 'wall_qps' else
                      r[metric.removesuffix('_us') + '_ns'] / 1000 if metric.endswith('_us') else r[metric] for r in rows]
            for label, function in (('min', min), ('median', statistics.median), ('max', max)):
                entry[metric + '_' + label] = function(values)
        summary.append(entry)
    for relative, sha in manifest['artifacts'].items():
        if digest(output / relative) != sha:
            raise ValueError('artifact mismatch: ' + relative)
    if json.loads((output / 'summary.json').read_text(encoding='utf-8')) != summary:
        raise ValueError('summary reconstruction mismatch')
    print('KNN_EVIDENCE_VERIFIED: measurements=%d reopens=%d performance=%s' %
          (len(measurements), len(measurements), manifest['performance_eligible']), flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=('ordinary', 'asan', 'tsan'), default='ordinary')
    parser.add_argument('--quick', action='store_true', help='Small correctness matrix; no speedup claim')
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--audit', type=Path)
    args = parser.parse_args()
    if args.audit:
        audit(args.audit); return
    if args.profile != 'ordinary' and not args.quick:
        parser.error('sanitizer profiles require --quick')
    if args.profile == 'tsan' and os.name == 'nt':
        parser.error('TSAN requires Linux')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = (args.output or ROOT / 'report/optimization' / ('knn_matrix_' + stamp)).resolve()
    build = (args.build_dir or ROOT / ('build-knn-' + args.profile + '-' + stamp)).resolve()
    if not output.is_relative_to(ROOT) or not build.is_relative_to(ROOT):
        parser.error('build and evidence must stay within the project')
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    if os.name == 'nt':
        from build_local import build_env
        env, cmake, ninja = build_env()
        compiler = ['-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl']
    else:
        cmake, ninja = shutil.which('cmake'), shutil.which('ninja')
        compiler = ['-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++']
    env['TSAN_OPTIONS'] = 'halt_on_error=1:exitcode=66'
    env['ASAN_OPTIONS'] = 'halt_on_error=1'
    executable = build / ('ltm_knn_bench.exe' if os.name == 'nt' else 'ltm_knn_bench')
    regimes = [('evicting', 12 if args.quick else 16), ('resident', 128 if args.quick else 256)]
    ks, readers, repetitions = [1, 16, 64], [1, 4] if args.quick else [1, 2, 4], 1 if args.quick else 3
    base = dict(records=512 if args.quick else 1024, ops=48 if args.quick else 4096, warmup=32 if args.quick else 256, seed=42)
    manifest = dict(schema_version=1, profile=args.profile, quick=args.quick, performance_eligible=args.profile == 'ordinary' and not args.quick,
                    build=str(build), executable=str(executable), regimes=regimes, ks=ks, readers=readers,
                    repetitions=repetitions, expected_measurements=len(regimes) * len(ks) * len(readers) * repetitions,
                    sources={}, commands=[], artifacts={}, completed=False, started_utc=stamp)
    def persist(): save(output / 'manifest.json', manifest)
    for path in source_files():
        relative = path.relative_to(ROOT).as_posix(); target = output / 'source' / relative
        target.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(path, target)
        manifest['sources'][relative] = digest(path)
    persist()
    def run(name, command, expected=None, timeout=900, expected_exit=0):
        print('KNN_STAGE: ' + name, flush=True)
        row = dict(name=name, command=list(map(str, command)), expected_exit=expected_exit)
        manifest['commands'].append(row); persist(); tick = time.perf_counter()
        try:
            result = subprocess.run(row['command'], cwd=ROOT, env=env, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            for suffix in ('stdout', 'stderr'): (output / (name + '.' + suffix + '.txt')).write_bytes(getattr(error, suffix) or b'')
            row['timed_out'] = True; persist(); raise
        for suffix in ('stdout', 'stderr'):
            path = output / (name + '.' + suffix + '.txt'); path.write_bytes(getattr(result, suffix)); row[suffix + '_sha256'] = digest(path)
        row.update(exit_code=result.returncode, seconds=time.perf_counter() - tick)
        if result.returncode != expected_exit:
            persist(); raise ValueError(name + ' failed: ' + result.stderr.decode('utf-8', errors='replace')[-3000:] + result.stdout.decode('utf-8', errors='replace')[-3000:])
        if expected is not None:
            row['expected'] = expected; row['result'] = parse_result(result.stdout.decode('utf-8', errors='replace'), expected)
        persist(); return row.get('result')
    if args.profile == 'tsan':
        from accept_tsan import PROBE
        probe = PROBE.replace('ready.fetch_add(1)', 'ready.fetch_add(1, std::memory_order_relaxed)')
        probe = probe.replace('ready.load()', 'ready.load(std::memory_order_relaxed)')
        probe = probe.replace('i < 1000;', 'i < 100000;').replace('value == 3000', 'value == 300000')
        (output / 'control.cc').write_text(probe, encoding='utf-8')
        for name, flags in (('race', []), ('clean', ['-DCLEAN=1'])):
            target = output / ('control_' + name)
            run('compile_' + name, ['clang++', '-std=c++17', '-O1', '-g', '-fsanitize=thread', '-pthread',
                                  *flags, output / 'control.cc', '-o', target])
            run('runtime_' + name, [target], expected_exit=66 if name == 'race' else 0)
        if 'WARNING: ThreadSanitizer: data race' not in (output / 'runtime_race.stderr.txt').read_text(encoding='utf-8'):
            raise ValueError('TSAN positive control did not diagnose a data race')
        for name in ('control.cc', 'control_race', 'control_clean'):
            manifest['artifacts'][name] = digest(output / name)
    if not args.skip_build:
        if build.exists(): raise ValueError('refuse to change an existing build; choose a new directory')
        run('configure', [cmake, '-S', ROOT / 'tools/knn_bench', '-B', build, '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + ninja,
                          '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DPython3_EXECUTABLE=' + sys.executable,
                          '-DLTM_ENABLE_ASAN=' + ('ON' if args.profile == 'asan' else 'OFF'),
                          '-DLTM_ENABLE_TSAN=' + ('ON' if args.profile == 'tsan' else 'OFF'), *compiler])
        run('build', [cmake, '--build', build, '--target', 'ltm_knn_bench', '--parallel', '4'])
    cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
    for key, desired in (('LTM_ENABLE_ASAN', args.profile == 'asan'), ('LTM_ENABLE_TSAN', args.profile == 'tsan')):
        if key + ':BOOL=' + ('ON' if desired else 'OFF') not in cache: raise ValueError('build profile mismatch')
    manifest['executable_sha256'] = digest(executable)
    if args.profile == 'asan' and os.name == 'nt':
        run('runtime_symbols', [shutil.which('dumpbin', path=env['PATH']), '/dependents', executable])
        if 'clang_rt.asan_dynamic-x86_64.dll' not in (output / 'runtime_symbols.stdout.txt').read_text(encoding='utf-8', errors='replace'):
            raise ValueError('ASAN runtime import absent')
    elif args.profile == 'tsan':
        run('runtime_symbols', ['nm', executable])
        if '__tsan_init' not in (output / 'runtime_symbols.stdout.txt').read_text(encoding='utf-8'):
            raise ValueError('TSAN runtime symbol absent')
    shutil.copyfile(build / 'CMakeCache.txt', output / 'CMakeCache.txt')
    shutil.copyfile(build / 'compile_commands.json', output / 'compile_commands.json')
    compile_units = json.loads((output / 'compile_commands.json').read_text(encoding='utf-8'))
    if args.profile != 'ordinary':
        flag = '/fsanitize=address' if os.name == 'nt' else '-fsanitize=' + ('address' if args.profile == 'asan' else 'thread')
        if any(flag not in unit['command'] for unit in compile_units): raise ValueError('uninstrumented compilation unit')
        manifest['instrumented_compile_units'] = len(compile_units)
    env['LTM_KNN_BENCH'] = str(executable); env['LTM_KNN_TEST_DIR'] = str(output / 'protocol-data')
    run('protocol_tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'scripts', '-p', 'test_knn_bench.py', '-v'])
    cases = [(regime, frames, k, n, rep) for regime, frames in regimes for k in ks for n in readers for rep in range(repetitions)]
    random.Random(20261016).shuffle(cases)  # Fixed shuffled order reduces monotonic order bias.
    collected = []
    for case_id, (regime, frames, k, n, rep) in enumerate(cases):
        settings = dict(base, regime=regime, frames=frames, k=k, readers=n)
        stem = '%03d_%s_k%d_t%d_r%d' % (case_id, regime, k, n, rep)
        directory = output / 'databases' / stem
        for stage in ('prepare', 'measure', 'verify'):
            expected = dict(settings, stage=stage)
            command = [executable, '--run-dir=' + str(directory)] + ['--%s=%s' % item for item in expected.items()]
            row = run(stem + '_' + stage, command, expected)
            if stage == 'measure': collected.append(row)
        for filename in ('pages.data', 'records.wal'):
            path = directory / filename; manifest['artifacts'][path.relative_to(output).as_posix()] = digest(path)
        persist()
    groups = {}
    for row in collected: groups.setdefault((row['regime'], row['frames'], row['k'], row['readers']), []).append(row)
    summary = []
    for key, rows in sorted(groups.items()):
        entry = dict(zip(('regime', 'frames', 'k', 'readers'), key)); entry['repetitions'] = len(rows)
        for metric in ('wall_qps', 'p50_us', 'p95_us', 'p99_us', 'page_reads', 'evictions'):
            values = [r['ops'] * 1e9 / r['wall_ns'] if metric == 'wall_qps' else
                      r[metric.removesuffix('_us') + '_ns'] / 1000 if metric.endswith('_us') else r[metric] for r in rows]
            for label, function in (('min', min), ('median', statistics.median), ('max', max)):
                entry[metric + '_' + label] = function(values)
        summary.append(entry)
    save(output / 'summary.json', summary)
    with (output / 'summary.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary[0])); writer.writeheader(); writer.writerows(summary)
    for name in ('summary.json', 'summary.csv', 'CMakeCache.txt', 'compile_commands.json'):
        manifest['artifacts'][name] = digest(output / name)
    manifest['completed'] = True; manifest['finished_utc'] = datetime.now(timezone.utc).isoformat(); persist()
    audit(output)
    print('KNN_OUTPUT: ' + str(output), flush=True)


if __name__ == '__main__':
    main()
