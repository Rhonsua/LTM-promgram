"""Build, test, compare checked/no-probe trajectories, and verify fresh-process persistence."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'report/acceptance/mixed_measure_20261016'


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument('--phase', choices=('ordinary', 'asan', 'tsan', 'all'), default='all')
    args = ap.parse_args()
    windows = os.name == 'nt'
    if args.phase == 'all':
        if not windows:
            ap.error('all orchestrates Windows plus WSL; use --phase tsan on Linux')
        # Sequential profiles keep ordinary benchmark observations separate from
        # the CPU/storage load of sanitizer tests. Every child records its own logs.
        for phase in ('ordinary', 'asan', 'tsan'):
            command = [sys.executable, '-B', str(Path(__file__).resolve()), '--phase', phase]
            if phase == 'tsan':
                linux_root = '/mnt/' + ROOT.drive[0].lower() + ROOT.as_posix()[2:]
                command = ['wsl.exe', '-d', 'Ubuntu', '--exec', 'python3', '-B',
                           linux_root + '/scripts/accept_mixed_measure.py', '--phase', phase]
            print('PROJECT_ACCEPT_PROFILE: ' + phase, flush=True)
            if subprocess.run(command, cwd=ROOT).returncode:
                return 1
        print('PROJECT_ACCEPT_ALL_PASSED', flush=True)
        return 0
    phase = args.phase
    if (phase == 'tsan') == windows:
        ap.error('ordinary/asan require Windows; tsan requires Linux')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%H%M%S%f')
    output = OUT / phase
    if output.exists(): output = OUT / (phase + '_' + stamp)
    output.mkdir(parents=True, exist_ok=False)
    build = ROOT / ('build-accept-mixed-' + phase + '-' + stamp)
    sources = [ROOT / 'CMakeLists.txt'] + sorted(p for folder in ('src', 'tests', 'scripts', 'demo', 'tools')
        for p in (ROOT / folder).rglob('*') if p.suffix in ('.cc', '.h', '.py', '.ps1'))
    manifest = dict(passed=False, phase=phase, build_directory=str(build), started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    source_sha256={p.relative_to(ROOT).as_posix(): digest(p) for p in sources}, runs=[])
    manifest_path = output / 'manifest.json'
    def save(): manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    for source in sources:
        target = output / 'source_snapshot' / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    env = dict(os.environ)
    if windows:
        from build_local import build_env
        env, cmake, ninja = build_env()
        ctest = str(Path(cmake).with_name('ctest.exe'))
        compiler = ['-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl']
    else:
        cmake, ninja, ctest = 'cmake', 'ninja', 'ctest'
        compiler = ['-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++']
        env['TSAN_OPTIONS'] = 'halt_on_error=1:exitcode=66'
        build.mkdir(exist_ok=True)
        log = build / 'log'
        if not log.exists() and not log.is_symlink():
            data = Path('/var/tmp') / ('ltm-mixed-accept-' + stamp)
            data.mkdir(exist_ok=False)
            log.symlink_to(data, target_is_directory=True)
        if not log.is_symlink(): raise ValueError('refuse a non-symlink Linux test log folder')
        manifest['persistent_test_data'] = str(log.resolve())
    env['LTM_MIXED_TEST_BUILD'] = build.name
    save()
    def run(stage, command, expected=0, cwd=ROOT, timeout=1800):
        print('PROJECT_ACCEPT_STAGE: ' + phase + '/' + stage, flush=True)
        row = dict(stage=stage, command=list(map(str, command)), passed=False)
        manifest['runs'].append(row); save(); tick = time.perf_counter()
        try:
            result = subprocess.run(row['command'], cwd=cwd, env=env, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            (output / (stage + '.stdout.txt')).write_bytes(error.stdout or b'')
            (output / (stage + '.stderr.txt')).write_bytes(error.stderr or b'')
            row['timed_out'] = True; save(); raise
        (output / (stage + '.stdout.txt')).write_bytes(result.stdout)
        (output / (stage + '.stderr.txt')).write_bytes(result.stderr)
        row.update(exit_code=result.returncode, expected_exit=expected, seconds=time.perf_counter() - tick,
                   passed=result.returncode == expected)
        save()
        if not row['passed']:
            print(result.stdout.decode('utf-8', errors='replace')[-4500:])
            print(result.stderr.decode('utf-8', errors='replace')[-4500:])
            raise ValueError(stage + ' failed; inspect retained logs')
        return result
    try:
        if phase == 'tsan':
            from accept_tsan import PROBE
            probe = PROBE.replace('ready.fetch_add(1)', 'ready.fetch_add(1, std::memory_order_relaxed)')
            probe = probe.replace('ready.load()', 'ready.load(std::memory_order_relaxed)')
            probe = probe.replace('i < 1000;', 'i < 100000;').replace('value == 3000', 'value == 300000')
            (output / 'control.cc').write_text(probe, encoding='utf-8')
            for name, flags in (('race', []), ('clean', ['-DCLEAN=1'])):
                run('compile_' + name, ['clang++', '-std=c++17', '-O1', '-g', '-fsanitize=thread', '-pthread',
                    *flags, output / 'control.cc', '-o', output / name])
                control = run('control_' + name, [output / name], expected=66 if name == 'race' else 0)
                if name == 'race' and b'WARNING: ThreadSanitizer: data race' not in control.stderr:
                    raise ValueError('missing real TSAN positive-control report')
            manifest['runtime_controls_proven'] = True
        run('configure', [cmake, '-S', ROOT, '-B', build, '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + ninja,
            '-DCMAKE_BUILD_TYPE=RelWithDebInfo', *compiler, '-DLTM_WERROR=ON',
            '-DLTM_ENABLE_ASAN=' + ('ON' if phase == 'asan' else 'OFF'),
            '-DLTM_ENABLE_TSAN=' + ('ON' if phase == 'tsan' else 'OFF')])
        run('build', [cmake, '--build', build, '--parallel', '4'])
        for name in ('CMakeCache.txt', 'compile_commands.json'):
            shutil.copy2(build / name, output / name)
        extension = '.exe' if windows else ''
        shutil.copy2(build / ('ltm_bench' + extension), output / ('ltm_bench' + extension))
        if phase == 'asan':
            manifest['asan_import_present'] = b'clang_rt.asan' in (build / 'ltm_bench.exe').read_bytes()
            if not manifest['asan_import_present']: raise ValueError('missing actual ASAN runtime import')
        if phase == 'tsan':
            commands = json.loads((build / 'compile_commands.json').read_text())
            if not commands or not all('-fsanitize=thread' in row['command'] for row in commands):
                raise ValueError('missing translation-unit instrumentation')
            symbols = run('symbols', ['nm', build / 'ltm_bench'])
            if b'__tsan_init' not in symbols.stdout: raise ValueError('missing actual TSAN runtime')
            manifest['instrumented_translation_units'] = len(commands)
        ctest_command = [ctest, '--test-dir', build, '--output-on-failure', '-E', 'validate']
        if phase == 'tsan': ctest_command += ['-R', '^test_(mixed_reference|rtree_diff|hybrid_concurrency)$']
        result = run('ctest', ctest_command)
        expected_tests = 3 if phase == 'tsan' else 20
        if ('0 tests failed out of ' + str(expected_tests)).encode() not in result.stdout:
            raise ValueError('unexpected ctest count')
        manifest['ctest_count'] = expected_tests
        shutil.copy2(build / 'Testing/Temporary/LastTest.log', output / 'LastTest.log')
        for name in ('ltm_validate_bplus', 'ltm_validate_rtree'):
            run(name, [build / (name + extension), '--quiet'], cwd=build)
        run('mixed_protocol', [sys.executable, '-B', ROOT / 'scripts/test_mixed_bench.py', '-v'])
        run('readonly_protocol', [sys.executable, '-B', ROOT / 'scripts/test_bench.py', '-v'])
        run('layering', [sys.executable, '-B', ROOT / 'scripts/check_layering.py'])
        run('ascii', [sys.executable, '-B', ROOT / 'scripts/check_ascii_literals.py'])
        run('pipeline', [sys.executable, '-B', ROOT / 'scripts/run_pipeline.py', '--skip-build',
                         '--build-dir', build, '--output-dir', output / 'pipeline'])
        for trajectory in ('checked', 'measure'):
            run('mixed_' + trajectory, [sys.executable, '-B', ROOT / 'scripts/mixed_bench.py', '--build-dir', build,
                '--output-dir', output / trajectory, '--trajectory', trajectory, '--records', '512', '--ops', '300',
                '--frames', '12', '128', '--repeats', '3', '--warmup-ops', '30'], timeout=3600)
        from mixed_bench import HASHES
        reference = None
        pairs = 0
        for frames in (12, 128):
            for repeat in (1, 2, 3):
                records = [json.loads((output / trajectory / ('frames%d_repeat%d' % (frames, repeat)) /
                            (stage + '.result.json')).read_text()) for trajectory in ('checked', 'measure')
                           for stage in ('run', 'verify')]
                hashes = [tuple(row[key] for key in HASHES) for row in records]
                if len(set(hashes)) != 1 or reference is not None and hashes[0] != reference:
                    raise ValueError('cross-trajectory input or final-record mismatch')
                reference = hashes[0]; pairs += 1
        manifest['trajectory_pairs'] = pairs
        manifest['cross_trajectory_fingerprints'] = dict(zip(HASHES, reference))
        names = list(build.glob('*.exe')) + list(build.glob('*.lib')) if windows else [build / name for name in
            ('ltm_bench', 'ltm_pipeline', 'ltm_validate_bplus', 'ltm_validate_rtree', 'test_mixed_reference',
             'test_rtree_diff', 'test_hybrid_concurrency')] + list(build.glob('*.a'))
        manifest['binary_sha256'] = {p.relative_to(ROOT).as_posix(): digest(p) for p in names}
        manifest['passed'] = True
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        manifest['error'] = str(error)
        print('PROJECT_ACCEPT_ERROR: ' + str(error), flush=True)
    finally:
        manifest['source_unchanged'] = all(digest(ROOT / name) == sha for name, sha in manifest['source_sha256'].items())
        manifest['passed'] = manifest['passed'] and manifest['source_unchanged']
        manifest['completed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat(); save()
    print('PROJECT_ACCEPT_VERDICT: ' + ('passed' if manifest['passed'] else 'failed'), flush=True)
    print('PROJECT_ACCEPT_REPORT: ' + str(output), flush=True)
    return 0 if manifest['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
