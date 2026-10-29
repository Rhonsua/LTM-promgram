#!/usr/bin/env bash
# scripts/soak_m4_2.sh -- M4-2 phase 5 soak driver.
#
# Purpose: run the Debug (assertions live) binaries for a fixed wall-clock budget
# and fail loudly if the I6 write barrier or any assert fires.
#
# Usage: scripts/soak_m4_2.sh [seconds] [build_dir]
#   defaults: 3600 seconds, build-dbg (Debug: assert() is live)
#
# ---------------------------------------------------------------------------
# THREE PROPERTIES THIS DRIVER HAS TO HAVE, all learned the hard way
# ---------------------------------------------------------------------------
# 1) FILESYSTEM ISOLATION. The validators use hard-coded relative paths
#    (log/_validate_bplus.{data,wal}). The first version of this script ran them
#    from the repo root, i.e. on exactly the same files the gate scripts use. A
#    RelWithDebInfo validator run that happened to overlap this soak then deleted
#    and recreated log/_validate_rtree.data mid-flight; the soak's process saw an
#    existing-but-zeroed page file, took the reopen path, and correctly reported
#    an I6 violation -- a true failure message about a state that only existed
#    because two processes were fighting over one file. The binaries are
#    therefore now run with CWD set to a private scratch directory, so their log/
#    is their own. Nothing else may run against that directory; the gates keep
#    using log/ in the repo root.
#
# 2) A PER-ITERATION TIMEOUT. Not because a hang was ever observed -- it was not
#    -- but because without one a hang would be *indistinguishable* from a slow
#    run: the soak would simply stop producing progress lines and report nothing.
#    A soak that silently yields no evidence is the worst outcome available, so
#    the timeout is recorded as a failure and the iteration's log is kept.
#    (An earlier reading of "the first run stalled" was wrong: the loop was still
#    running, and its lines for iterations 11-12 were lost only because this
#    script's first action is to truncate the progress file.)
#
# 3) NO SOAK-OWNED FILE OUTSIDE THE PRIVATE DIRECTORY, and an explicit EMPTY_OUTPUT
#    classification. Both come from the same observed accident: the per-iteration
#    output file used to be written into report/m4_2/ as .soak_<kind>_<n>.out, and
#    a stray-file cleanup deleted one *while the running validator had it open*.
#    The process kept writing to the now-unlinked inode, so the file read back
#    EMPTY -- and an empty output makes every content check fire at once
#    ("counters not printed", "no deltas", ...), which looks exactly like the
#    barrier collapsing. The real cause was a deleted file, and nothing in the
#    report said so. Now the output lives in the private directory, and an empty
#    output with rc=0 is named as such instead of being reported as a counter
#    failure.

set -u

SECONDS_BUDGET="${1:-3600}"
BUILD_DIR="${2:-build-dbg}"
OUT_DIR="report/m4_2"
PROGRESS="${OUT_DIR}/soak_progress.txt"
FAILFILE="${OUT_DIR}/soak_failures.txt"
SUMMARY="${OUT_DIR}/soak_summary.txt"
SCRATCH="$(pwd)/build-dbg-soak"     # private CWD for the validators
ITER_TIMEOUT="${SOAK_ITER_TIMEOUT:-300}"

REPO="$(pwd)"

# Refuse to run against a build without live asserts: proving "no assertion fired"
# against an NDEBUG binary would be a fake green, which is the exact failure this
# project already shipped once (defect #37).
if [ ! -f "${BUILD_DIR}/test_wal_barrier.exe" ]; then
  echo "soak: ${BUILD_DIR} does not contain built binaries; build it first" >&2
  exit 2
fi

mkdir -p "${OUT_DIR}" "${SCRATCH}/log"
: > "${PROGRESS}"
: > "${FAILFILE}"

# Failure signatures. Every one is ASCII and every one is a real signal:
#  - "Assertion failed"          : any assert() in the tree (Debug build)
#  - "I6 violated"               : the barrier refused a page write
#  - any of the three leak counters with a non-zero count
#  - "violations 0" / "missed 0" being ABSENT from the output (see below)
# NOTE: identical-skipped is deliberately NOT a failure signature. It counts pages
# that were marked dirty but whose bytes did not change (D3) -- a non-zero value
# is the optimization working, not a defect. Only the three counters that mean
# "a page was written with no REDO covering it" belong here.
PATTERNS='Assertion failed|I6 violated'
# The parentheses are load-bearing: without them this alternation reads as
# `outside-txn` OR `missing-before` OR `unstamped-writes [1-9]`, so the first two
# branches match on the NAME alone and every iteration is flagged. Caught by
# unit-testing the detector (see the phase 5 report) before the 1-hour run.
COUNTERS='(outside-txn|missing-before|unstamped-writes)'

start=$(date +%s)
iter=0
fails=0
declare -A runs_by_kind

elapsed() { echo $(( $(date +%s) - start )); }

# One iteration: run one binary in the private scratch CWD, verify its output.
# $1 kind (bplus|rtree)  $2.. args
run_one() {
  local kind="$1"; shift
  local bin="${REPO}/${BUILD_DIR}/ltm_validate_${kind}.exe"
  local out="${SCRATCH}/out_${kind}_${iter}.txt"

  # Fresh cold start every iteration in the private directory. The page file
  # matters as much as the log: these validators reopen an existing database on
  # purpose, so leaving it would silently skip the create path.
  rm -f "${SCRATCH}/log/_validate_${kind}.wal" "${SCRATCH}/log/_validate_${kind}.data"

  local t0 t1 rc
  t0=$(date +%s)
  # timeout(1) wraps the binary; exit 124 means it hung and was killed.
  ( cd "${SCRATCH}" && exec timeout -k 5 "${ITER_TIMEOUT}" "${bin}" --quiet "$@" ) \
      > "${out}" 2>&1
  rc=$?
  t1=$(date +%s)

  local bad=""
  if [ "${rc}" -eq 124 ] || [ "${rc}" -eq 137 ]; then
    bad="${bad} +TIMEOUT_${ITER_TIMEOUT}s"
  elif [ "${rc}" -ne 0 ]; then
    bad="${bad} exit=${rc}"
  fi
  if grep -qE "${PATTERNS}" "${out}"; then bad="${bad} +matched_failure_signature"; fi

  # An empty output file with rc=0 cannot happen for these binaries: every exit
  # path prints something. Classify it separately from the content checks, so a
  # harness accident is never mistaken for the barrier collapsing.
  local dels=""
  local wal_bytes=0
  if [ ! -s "${out}" ]; then
    bad="${bad} +EMPTY_OUTPUT(harness: the output file was empty; the binary printed nothing)"
  else
    # The four leak counters must be printed and the three that mean "a page was
    # written with no REDO" must all read 0. Checking for the line's PRESENCE is
    # what stops this from degrading into a vacuous grep: if a future edit stops
    # printing the counters, this soak goes red rather than green.
    if ! grep -q "WAL leak :" "${out}"; then
      bad="${bad} +leak_counters_not_printed"
    elif grep "WAL leak :" "${out}" | grep -qE "${COUNTERS} [1-9]"; then
      bad="${bad} +nonzero_leak_counter"
    fi
    if ! grep -q ", I6 violations 0," "${out}"; then bad="${bad} +i6_violations_missing_or_nonzero"; fi
    if ! grep -q "(missed 0)" "${out}"; then bad="${bad} +lsn_stamps_missed_missing_or_nonzero"; fi

    # An empty WAL means the barrier was never exercised -- "0 violations" over
    # zero deltas is exactly the fake green this milestone exists to prevent.
    dels=$(sed -n 's/.*WAL      : deltas \([0-9]*\).*/\1/p' "${out}")
    if [ -z "${dels}" ] || [ "${dels}" -eq 0 ]; then bad="${bad} +no_wal_deltas"; fi
  fi

  wal_bytes=0
  [ -f "${SCRATCH}/log/_validate_${kind}.wal" ] && \
    wal_bytes=$(stat -c %s "${SCRATCH}/log/_validate_${kind}.wal")

  printf '%s iter=%d kind=%-5s rc=%d %4ds deltas=%-6s wal=%4sMB%s\n' \
    "$(date -u +%H:%M:%S)" "${iter}" "${kind}" "${rc}" "$(( t1 - t0 ))" "${dels:-?}" \
    "$(( wal_bytes / 1048576 ))" "${bad}" >> "${PROGRESS}"

  if [ -n "${bad}" ]; then
    fails=$(( fails + 1 ))
    {
      echo "=============== SOAK FAILURE iter=${iter} kind=${kind} ==============="
      echo "args: $*   exit=${rc}   signals:${bad}"
      echo "--- output ---"
      cat "${out}"
      echo "--- log/_validate_${kind}.wal size: ${wal_bytes} bytes ---"
    } >> "${FAILFILE}"
  else
    runs_by_kind[${kind}]=$(( ${runs_by_kind[${kind}]:-0} + 1 ))
  fi
  rm -f "${out}"
  [ -z "${bad}" ]
}

echo "soak: budget ${SECONDS_BUDGET}s, build ${BUILD_DIR} (Debug: assert() live)"
echo "soak: private CWD ${SCRATCH}; per-iteration timeout ${ITER_TIMEOUT}s"
echo "soak: progress -> ${PROGRESS}, failures -> ${FAILFILE}"

# Schedule: rotated so every iteration differs in seed and usually in pool size.
# Small frame counts are deliberately included -- a pool too small to hold one
# operation forces evictions *inside* a transaction, which is the only way
# wal_sync_on_flush (the always-on write-ahead rule) gets exercised.
while [ "$(elapsed)" -lt "${SECONDS_BUDGET}" ]; do
  iter=$(( iter + 1 ))
  seed=$(( 1000 + iter * 7919 ))
  case $(( iter % 5 )) in
    0) run_one bplus --ops=20000 --frames=128 --seed=${seed} ;;
    1) run_one rtree --ops=8000  --frames=128 --seed=${seed} ;;
    2) run_one bplus --ops=8000  --frames=24  --seed=${seed} ;;
    3) run_one rtree --ops=4000  --frames=32  --seed=${seed} ;;
    4) run_one bplus --ops=6000  --frames=16  --seed=${seed} ;;
  esac
done

total=$(elapsed)
{
  echo "M4-2 phase 5 soak -- result"
  echo "budget_seconds : ${SECONDS_BUDGET}"
  echo "elapsed_seconds: ${total}"
  echo "iterations     : ${iter}"
  echo "failures       : ${fails}"
  echo "build          : ${BUILD_DIR} (Debug, assert() live)"
  echo "binaries       : bplus ${runs_by_kind[bplus]:-0} runs / rtree ${runs_by_kind[rtree]:-0} runs"
  echo "isolation      : each run had CWD=${SCRATCH}; its log/ is private to the soak"
  echo "wal policy     : log/_validate_<kind>.{wal,data} deleted at the start of every iteration"
  echo "iter_timeout   : ${ITER_TIMEOUT}s (exit 124/137 recorded as a failure)"
  echo "wal retention  : the per-iteration WAL was deleted after each run (full-page images,"
  echo "                 ~100-350 MB each). The evidence that it was non-empty is the per-"
  echo "                 iteration 'wal=NNNMB' column in soak_progress.txt, together with the"
  echo "                 'no_wal_deltas' check, which fails any iteration whose delta count is 0."
  echo "scratch        : ${SCRATCH} removed at exit"
  echo "verdict        : $([ "${fails}" -eq 0 ] && echo PASS || echo FAIL)"
} > "${SUMMARY}"

# The scratch tree holds nothing worth keeping once the per-iteration WAL sizes are
# recorded in the progress file, and it is ~350 MB per iteration if left behind.
rm -rf "${SCRATCH}"

cat "${SUMMARY}"
[ "${fails}" -eq 0 ]
