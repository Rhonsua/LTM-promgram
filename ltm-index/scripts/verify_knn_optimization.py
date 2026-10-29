"""Retain build and test evidence for the KNN reader optimization."""
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

def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--phase', choices=('baseline', 'ordinary', 'asan', 'debug', 'tsan'), required=True)
    args = parser.parse_args()
    phase = args.phase
    output = ROOT / 'report/optimization/knn_parallel_20261008' / phase
    if output.exists():
        output = output.with_name(phase + '_' + datetime.datetime.now(datetime.timezone.utc).strftime('%H%M%S%f'))
    output.mkdir(parents=True, exist_ok=False)
    windows = os.name == 'nt'
    if phase == 'tsan' and windows:
        parser.error('TSAN requires Linux')
    sources = [ROOT / 'CMakeLists.txt']
    for folder in ('src', 'tests', 'scripts', 'demo', 'tools'):
        sources.extend(p for p in (ROOT / folder).rglob('*') if p.suffix in ('.cc', '.h', '.py'))
    manifest = dict(passed=False, phase=phase, started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    source_sha256={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}, runs=[])
    def save():
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    if windows:
        from build_local import build_env
        env, cmake, ninja = build_env()
        ctest = str(Path(cmake).with_name('ctest.exe'))
        compiler = ['-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl']
        extension = '.exe'
    else:
        env, cmake, ninja, ctest = dict(os.environ), 'cmake', 'ninja', 'ctest'
        compiler = ['-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++']
        env['TSAN_OPTIONS'] = 'halt_on_error=1:exitcode=66'
        extension = ''
    build = ROOT / ('build-opt-knn' if phase in ('baseline', 'ordinary') else 'build-opt-knn-' + phase)
    manifest['build_directory'] = str(build)
    for source in sources:
        snapshot = output / 'source_snapshot' / source.relative_to(ROOT)
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, snapshot)
    if phase == 'tsan':
        build.mkdir(exist_ok=True)
        data_path = Path('/var/tmp') / ('ltm-knn-tsan-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%f'))
        data_path.mkdir(exist_ok=False)
        log_path = build / 'log'
        if log_path.exists() or log_path.is_symlink():
            if not log_path.is_symlink():
                raise RuntimeError('refuse to replace an existing Linux test log folder')
        else:
            log_path.symlink_to(data_path, target_is_directory=True)
        manifest['test_data_directory'] = str(log_path.resolve())
    save()
    def run(stage, argv, expected=0, cwd=ROOT, timeout=1800):
        print('KNN_STAGE: ' + phase + '/' + stage, flush=True)
        started = time.perf_counter()
        try:
            completed = subprocess.run(list(map(str, argv)), cwd=cwd, env=env, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            (output / (stage + '.stdout.txt')).write_bytes(error.stdout or b'')
            (output / (stage + '.stderr.txt')).write_bytes(error.stderr or b'')
            manifest['runs'].append(dict(stage=stage, command=list(map(str, argv)), timed_out=True, passed=False))
            save()
            raise
        (output / (stage + '.stdout.txt')).write_bytes(completed.stdout)
        (output / (stage + '.stderr.txt')).write_bytes(completed.stderr)
        manifest['runs'].append(dict(stage=stage, command=list(map(str, argv)), cwd=str(cwd),
                                     exit_code=completed.returncode, expected_exit=expected,
                                     seconds=time.perf_counter() - started, passed=completed.returncode == expected))
        save()
        if completed.returncode != expected:
            print(completed.stdout.decode('utf-8', errors='replace')[-4500:])
            print(completed.stderr.decode('utf-8', errors='replace')[-4500:])
            raise RuntimeError(stage + ' failed; inspect retained logs')
        return completed
    if phase == 'tsan':
        from accept_tsan import PROBE
        (output / 'control.cc').write_text(PROBE, encoding='utf-8')
        for name, flags in (('race', []), ('clean', ['-DCLEAN=1'])):
            run('compile_control_' + name, ['clang++', '-std=c++17', '-O1', '-g', '-fsanitize=thread',
                '-fno-omit-frame-pointer', '-pthread', *flags, output / 'control.cc', '-o', output / name])
            control = run('control_' + name, [output / name], expected=66 if name == 'race' else 0)
            if name == 'race' and b'WARNING: ThreadSanitizer: data race' not in control.stderr:
                raise RuntimeError('TSAN race control did not detect the race')
        manifest['runtime_controls_proven'] = True
        save()
    run('configure', [cmake, '-S', ROOT, '-B', build, '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + ninja,
        '-DCMAKE_BUILD_TYPE=' + ('Debug' if phase == 'debug' else 'RelWithDebInfo'), *compiler,
        '-DLTM_ENABLE_ASAN=' + ('ON' if phase == 'asan' else 'OFF'),
        '-DLTM_ENABLE_TSAN=' + ('ON' if phase == 'tsan' else 'OFF'), '-DLTM_WERROR=ON'])
    build_args = [cmake, '--build', build, '--parallel', '4']
    if phase == 'baseline':
        build_args += ['--target', 'test_hybrid_concurrency']
    elif phase in ('debug', 'tsan'):
        build_args += ['--target', 'test_hybrid_concurrency', 'test_rtree_diff', 'test_c1_replay', 'test_write_abort',
                       'ltm_validate_bplus', 'ltm_validate_rtree']
    run('build', build_args)
    for name in ('CMakeCache.txt', 'compile_commands.json'):
        shutil.copy2(build / name, output / name)
    if phase == 'baseline':
        before = run('overlap_regression', [build / ('test_hybrid_concurrency' + extension),
                     '--filter=KnnReadersOverlapAndNestedQueriesRemainIndependent'], expected=1, cwd=build)
        if b'[FAIL]' not in before.stderr or b'KnnReadersOverlapAndNestedQueriesRemainIndependent' not in before.stdout:
            raise RuntimeError('expected serialization regression was not exercised')
        shutil.copy2(build / ('test_hybrid_concurrency' + extension), output / ('test_hybrid_concurrency' + extension))
    else:
        if phase == 'asan':
            manifest['asan_import_present'] = b'clang_rt.asan' in (build / 'test_hybrid_concurrency.exe').read_bytes()
            save()
            if not manifest['asan_import_present']:
                raise RuntimeError('missing ASAN runtime import')
        test_args = [ctest, '--test-dir', build, '--output-on-failure', '-E', 'validate']
        if phase in ('debug', 'tsan'):
            test_args += ['-R', '^test_(hybrid_concurrency|rtree_diff|c1_replay|write_abort)$']
        result = run('ctest', test_args)
        expected_count = 4 if phase in ('debug', 'tsan') else 20
        if ('0 tests failed out of ' + str(expected_count)).encode() not in result.stdout:
            raise RuntimeError('test count or success marker mismatch')
        manifest['ctest_count'] = expected_count
        for name in ('ltm_validate_bplus', 'ltm_validate_rtree'):
            run(name, [build / (name + extension), '--quiet'], cwd=build)
        if phase == 'tsan':
            commands = json.loads((build / 'compile_commands.json').read_text())
            if not all('-fsanitize=thread' in row['command'] for row in commands):
                raise RuntimeError('missing TSAN instrumentation')
            run('runtime_symbols', ['nm', build / 'test_hybrid_concurrency'])
            if b'__tsan_init' not in (output / 'runtime_symbols.stdout.txt').read_bytes():
                raise RuntimeError('missing TSAN runtime')
            manifest['instrumented_translation_units'] = len(commands)
        run('layering', [sys.executable, '-B', ROOT / 'scripts/check_layering.py'])
        run('ascii', [sys.executable, '-B', ROOT / 'scripts/check_ascii_literals.py'])
    manifest['passed'] = True
    manifest['completed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save()
    print('KNN_VERIFY_PASSED: ' + phase, flush=True)

if __name__ == '__main__':
    main()
