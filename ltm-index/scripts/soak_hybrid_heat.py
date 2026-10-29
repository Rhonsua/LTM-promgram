#!/usr/bin/env python3
"""Repeat bounded, heat-enabled HybridIndex concurrency rounds with evidence."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
CASE = "HybridConcurrency.HeatEnabledMixedApisMatchCommittedRecords"


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--seconds", type=float, default=1800)
    parser.add_argument("--round-timeout", type=float, default=120)
    parser.add_argument("--build-dir", default="build-ninja")
    parser.add_argument("--output-dir", default="report/c1/heat/soak")
    args = parser.parse_args()
    if args.seconds <= 0 or args.round_timeout <= 0:
        parser.error("durations must be positive")
    out = (ROOT / args.output_dir).resolve()
    out.relative_to(ROOT)
    out.mkdir(parents=True, exist_ok=True)
    exe = ROOT / args.build_dir / "test_hybrid_concurrency.exe"
    if not exe.is_file():
        parser.error("build test_hybrid_concurrency before running")

    def hashes():
        files = [ROOT / "CMakeLists.txt", exe]
        files += [p for folder in ("src", "tests", "scripts")
                  for p in sorted((ROOT / folder).rglob("*"))
                  if p.suffix in (".h", ".cc", ".py")]
        return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in files}

    manifest = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "seconds_requested": args.seconds, "round_timeout": args.round_timeout,
                "command": [str(exe), "--filter=" + CASE],
                "source_sha256": hashes(), "rounds": [], "passed": False,
                "scope": "fresh database per bounded round; not one persistent 30-minute history"}
    started = time.monotonic()
    progress = started
    try:
        while time.monotonic() - started < args.seconds:
            number = len(manifest["rounds"]) + 1
            record = {"round": number}
            tick = time.monotonic()
            try:
                result = subprocess.run(manifest["command"], cwd=ROOT,
                                        capture_output=True, timeout=args.round_timeout)
                raw = result.stdout + b"\n--- stderr ---\n" + result.stderr
                record.update(exit_code=result.returncode, passed=(
                    result.returncode == 0 and b"HYBRID_HEAT readers=5 writers=2" in raw
                    and b"errors=0 checkpoints=4 validations=4" in raw
                    and b"1 passed, 0 failed" in raw))
            except subprocess.TimeoutExpired as exc:
                raw = (exc.stdout or b"") + (exc.stderr or b"")
                record.update(passed=False, error="round timeout")
            record["elapsed_seconds"] = time.monotonic() - tick
            (out / ("round_%04d.txt" % number)).write_bytes(raw)
            manifest["rounds"].append(record)
            (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            if not record["passed"]:
                raise RuntimeError("round %d failed" % number)
            if time.monotonic() - progress >= 30:
                print("heat soak: %d clean rounds, %.1fs elapsed" %
                      (number, time.monotonic() - started), flush=True)
                progress = time.monotonic()
        manifest["source_unchanged"] = hashes() == manifest["source_sha256"]
        manifest["passed"] = bool(manifest["rounds"]) and manifest["source_unchanged"]
    finally:
        manifest["elapsed_seconds"] = time.monotonic() - started
        manifest["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print("HEAT_SOAK_VERDICT: " + ("clean" if manifest["passed"] else "failed"), flush=True)
    return 0 if manifest["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
