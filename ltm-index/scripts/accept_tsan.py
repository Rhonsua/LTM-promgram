#!/usr/bin/env python3
"""Linux TSAN acceptance with runtime controls and retained raw evidence."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
PROBE = r'''#include <atomic>
#include <mutex>
#include <thread>
std::atomic<int> ready{0};
volatile int value = 0;
std::mutex mutex;
void work(int delta) {
  ready.fetch_add(1);
  while (ready.load() != 2) {}
  for (int i = 0; i < 1000; ++i) {
#ifdef CLEAN
    std::lock_guard<std::mutex> lock(mutex);
#endif
    value = value + delta;
  }
}
int main() {
  std::thread a(work, 1), b(work, 2);
  a.join(); b.join();
#ifdef CLEAN
  return value == 3000 ? 0 : 1;
#else
  return 0;
#endif
}
'''


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--output-dir")
    parser.add_argument("--build-dir", default="build-tsan-linux")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--scope", choices=("all", "concurrency"), default="all")
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("run inside Linux (including WSL2)")
    if args.jobs <= 0:
        parser.error("jobs must be positive")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    out = (ROOT / (args.output_dir or "report/c1/tsan/" + stamp)).resolve()
    build = (ROOT / args.build_dir).resolve()
    out.relative_to(ROOT); build.relative_to(ROOT)
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env["TSAN_OPTIONS"] = "halt_on_error=1:exitcode=66"
    sources = [ROOT / "CMakeLists.txt"]
    for folder in ("src", "tests", "scripts", "tools", "demo"):
        sources += sorted(p for p in (ROOT / folder).rglob("*") if p.suffix in (".cc", ".h", ".py"))
    manifest = {"passed": False, "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "arguments": vars(args), "platform": platform.platform(), "tsan_options": env["TSAN_OPTIONS"],
                "runs": [], "source_sha256": {str(p.relative_to(ROOT)): digest(p) for p in sources}}

    def save():
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    def run(name, command, cwd=ROOT, expected=0, marker=None, timeout=3600):
        print("TSAN_STAGE: " + name, flush=True)
        tick = time.perf_counter()
        try:
            result = subprocess.run(list(map(str, command)), cwd=cwd, env=env, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            (out / (name + ".stdout.txt")).write_bytes(error.stdout or b"")
            (out / (name + ".stderr.txt")).write_bytes(error.stderr or b"")
            manifest["runs"].append({"stage": name, "command": list(map(str, command)), "cwd": str(cwd),
                "exit_code": None, "expected_exit": expected, "passed": False, "timed_out": True,
                "seconds": time.perf_counter() - tick})
            save()
            raise
        (out / (name + ".stdout.txt")).write_bytes(result.stdout)
        (out / (name + ".stderr.txt")).write_bytes(result.stderr)
        passed = result.returncode == expected and (marker is None or marker.encode() in result.stdout + result.stderr)
        manifest["runs"].append({"stage": name, "command": list(map(str, command)), "cwd": str(cwd),
            "exit_code": result.returncode, "expected_exit": expected, "passed": passed,
            "seconds": time.perf_counter() - tick})
        save()
        if not passed:
            raise ValueError(name + " failed; inspect " + str(out))
        return result

    save()
    try:
        for name in ("cmake", "ctest", "ninja", "clang", "clang++", "nm"):
            if shutil.which(name) is None:
                raise ValueError("missing tool: " + name)
        run("environment", ["bash", "-lc", "uname -a; cat /etc/os-release; free -m; df -h .; clang++ --version; cmake --version; ninja --version"])
        (out / "control.cc").write_text(PROBE, encoding="utf-8")
        for name, extra in (("race", []), ("clean", ["-DCLEAN=1"])):
            run("compile_" + name, ["clang++", "-std=c++17", "-O1", "-g", "-fsanitize=thread",
                "-fno-omit-frame-pointer", "-pthread", *extra, out / "control.cc", "-o", out / name], timeout=120)
            run("control_" + name, [out / name], expected=66 if name == "race" else 0,
                marker="WARNING: ThreadSanitizer: data race" if name == "race" else None, timeout=120)
        manifest["runtime_proven"] = True
        run("configure", ["cmake", "-S", ROOT, "-B", build, "-G", "Ninja",
            "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DLTM_ENABLE_ASAN=OFF", "-DLTM_ENABLE_TSAN=ON", "-DLTM_ENABLE_FAULT_INJECT=OFF", "-DLTM_WERROR=ON"])
        run("build", ["cmake", "--build", build, "--parallel", args.jobs])
        for name in ("CMakeCache.txt", "compile_commands.json"):
            shutil.copy2(build / name, out / name)
        commands = json.loads((build / "compile_commands.json").read_text())
        if not commands or not all("-fsanitize=thread" in row["command"] for row in commands):
            raise ValueError("some translation units lack TSAN instrumentation")
        manifest["instrumented_translation_units"] = len(commands)
        run("runtime_symbols", ["nm", build / "test_c1_replay"], marker="__tsan_init")
        ctest = ["ctest", "--test-dir", build, "--output-on-failure"]
        if args.scope == "all":
            ctest += ["-E", "validate"]
        else:
            ctest += ["-R", "^test_(c0_latch|page_latch|c1_replay|hybrid_concurrency|write_abort)$"]
        result = run("ctest", ctest, marker="100% tests passed")
        count = re.search(rb"0 tests failed out of (\d+)", result.stdout)
        expected_count = 19 if args.scope == "all" else 5
        if count is None or int(count[1]) != expected_count:
            raise ValueError("wrong ctest scope/count")
        manifest["ctest_count"] = expected_count
        shutil.copy2(build / "Testing/Temporary/LastTest.log", out / "LastTest.log")
        for tree in ("bplus", "rtree"):
            run("validate_" + tree, [build / ("ltm_validate_" + tree), "--quiet"], cwd=build)
        # The CLI is portable even though the Windows one-command driver uses
        # MSVC setup and .exe names. Run fresh Linux child processes directly.
        database = out / "database"
        pipeline_results = []
        for stage in ("load", "verify"):
            row = run("pipeline_" + stage, [build / "ltm_pipeline", "--stage=" + stage,
                       "--run-dir=" + str(database)], marker="PIPELINE_RESULT ")
            lines = [line[16:] for line in row.stdout.decode().splitlines() if line.startswith("PIPELINE_RESULT ")]
            if len(lines) != 1:
                raise ValueError("pipeline output count mismatch")
            import run_pipeline
            pipeline_results.append(run_pipeline.parse_result(row.stdout.decode(), row.returncode, stage))
        if pipeline_results[0]["dataset_hash"] != pipeline_results[1]["dataset_hash"]:
            raise ValueError("cross-process dataset changed")
        manifest["pipeline_results"] = pipeline_results
        binaries = []
        for path in build.iterdir():
            if path.is_file():
                with path.open("rb") as stream:
                    if stream.read(4) == b"\x7fELF":
                        binaries.append(path)
        manifest["binary_sha256"] = {p.name: digest(p) for p in sorted(binaries)}
        manifest["source_unchanged"] = all(digest(ROOT / name) == value for name, value in manifest["source_sha256"].items())
        if not manifest["source_unchanged"]:
            raise ValueError("source changed during acceptance")
        manifest["passed"] = True
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        manifest["error"] = str(error)
        print("TSAN_ERROR: " + str(error), flush=True)
    finally:
        log = build / "Testing/Temporary/LastTest.log"
        if log.is_file():
            shutil.copy2(log, out / "LastTest.log")
        manifest["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        save()
    print("TSAN_VERDICT: " + ("passed" if manifest["passed"] else "failed"), flush=True)
    return 0 if manifest["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
