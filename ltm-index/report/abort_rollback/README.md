# M6 · write-transaction Abort must roll back memory, not just log

New test: `tests/concurrency/test_write_abort.cc` (4 cases).
Build: `build-ninja`, RelWithDebInfo, MSVC 14.51.36231 + Ninja.

Files in this directory:

| file | what it is |
| --- | --- |
| `abort_undo_red.txt` | verbatim output with the fix **disabled** — the gate going red |
| `abort_undo_green.txt` | verbatim output with the fix in place — 4 passed, 0 failed |
| `ctest_after_fix.txt` | full `ctest` run after the fix: 14/14 (121.1 s) |

Static checks on the final tree: `check_ascii_literals.py` — 73 files scanned, 0
with non-ASCII literals; `check_layering.py` — 46 source files, no layering or I/O
violations. Compiler warnings are errors (`/W4 /WX`), so the build is clean.

================================================================================
THE PROBLEM THIS GATE EXISTS FOR
================================================================================
`ScopedWriteOp::Abort` used to write one `kAbort` WAL record and stop. Crash
recovery was correct (the log says the transaction is gone, so Undo removes it),
but the **running process** kept the aborted transaction's changes.

This is not a theoretical window. The reachable path is `HybridIndex::Put`: it
writes the R-tree (spatial copy) and then the B+ tree (the record itself) inside
one transaction. If the second step fails, `ScopedWriteOp`'s destructor aborts —
and the R-tree entry stays in memory with no B+ counterpart, breaking I8 while
the process keeps serving reads from it.

`docs/status.md` already named this as a **precondition for C1**, not a backlog
item: under C0 the exclusive latch serialises writers so the window is closed;
C1 opens it.

The red run also showed the consequence is worse than "memory is inconsistent
until restart". `AbortedStateDoesNotSurviveReopen` fails too: after the abort,
`FlushAll` + reopen shows `entries=600` where the base is 400. A **clean**
shutdown writes the phantoms to the page file, and a clean shutdown runs no
recovery — so nothing ever corrects them.

================================================================================
WHY PAGE BEFORE-IMAGES WERE NOT ENOUGH
================================================================================
The transaction already kept a full-page before-image for every page it pinned
(`Impl::WriteOp::before`, used to build the REDO deltas). Replaying those back
into the frames restored the pages — and the test still failed, in a way that
was useful rather than mysterious:

    Validate: kCorruption: total entries in pages 400 != metadata count 600

The pages said 400, and the tree's own bookkeeping said 600. Three kinds of state
had to be rolled back, and only the first lives in pages:

1. **Page frames** — replay the before-image. Snapshot order (first pin inside
   the transaction) makes it exactly the transaction-start state.
2. **Allocator state** — `free_list_head` / `free_pages` / `next_unallocated`
   live on `Impl`, not in any page. `NewPage`/`FreePage` mutate them inside write
   transactions, so a page-image rollback cannot reach them. Not rolling them back
   leaves the free list and the allocation cursor pointing into the aborted
   transaction (invariant I7).
   Pages *allocated by* the aborted transaction are a special case: they have no
   usable before-image, because `NewPage` deliberately snapshots **before**
   `Init()` (snapshotting after would make before == after and land a page with
   `kInvalidLsn` on disk). Replaying that snapshot restores a frame of leftover
   garbage. The correct rollback is "this page never existed": drop the dirty bit
   and let the allocator hand the page number out again. (This one was not
   reasoned out — implementing it as "restore" instead is what produced the I6
   poisoning in the first draft, see below.)
3. **Derived tree state** — `IndexTree::Impl`'s `root`, `leftmost_leaf`,
   `height`, `num_entries`. These are in no page at all: the page-0 tree slot is
   only synced at Open/Close/Checkpoint, not per insert. So the buffer pool
   cannot reach them, and no amount of page-image fidelity will.

Item 3 is why the fix is not confined to the buffer pool. The buffer pool now
exposes a small participant protocol (`write_op_generation()` +
`RegisterWriteUndo()`): a tree registers its transaction-start state when it
*joins* a transaction, and the pool runs those callbacks (LIFO) on abort.
Registration happens before the tree changes anything, so what is captured is the
transaction-start state, not a half-applied one.

================================================================================
I6 INTERACTION — THE PART THAT LOOKS LIKE A TRAP AND IS NOT
================================================================================
Restoring a before-image restores the whole page **including the 32-byte header**,
so `page_lsn` goes back to its pre-transaction value as well. That is deliberate:
`FlushFrame`'s barrier rule is `page_lsn > flushed_lsn ⇒ wal->Sync()` first, so
lowering `page_lsn` can only make that condition *easier* to satisfy. A rollback
cannot manufacture a new I6 violation.

The inverse (restore the payload, leave the LSN) is the trap: the page content
would return to its pre-transaction state while its header kept claiming coverage
by a REDO record from an aborted transaction.

**No CLR is written, and that is correct.** A runtime abort produced no redo that
reached disk — every change is still in memory. The aborted transaction's REDO
records plus its `kAbort` send recovery down the Undo path, which agrees with what
the memory rollback produced. CLRs are for *recovery-time* Undo, where updates
have already been replayed onto pages.

Ordering inside `AbortWriteOp` matters and is commented at the call site: memory
is rolled back **before** the `kAbort` record is written. The other order opens a
narrow window where the log has certified "this transaction is gone" while the
pages still carry it. C0's exclusive latch keeps that window shut; C1 will not.

================================================================================
FALSIFIABILITY EXPERIMENT — the gate must be able to go red
================================================================================
Single variable: in `BufferPool::AbortWriteOp`, the call

    const Status rolled_back = im.RollbackWriteOp(this);

was replaced with `Status::Ok()`, disabling the entire rollback (pages, allocator,
and participant callbacks) while touching nothing else. This reproduces exactly
the pre-fix behaviour.

Result, verbatim from `abort_undo_red.txt` (4 cases, 1 passed, 3 failed):

    [FAIL] WriteAbort.RollsBackMemoryPages
           EXPECT_EQ failed: size_t(0) == still_there (left=0, right=200)
    [FAIL] WriteAbort.AbortedStateDoesNotSurviveReopen
           EXPECT_EQ failed: size_t(0) == CountPresent(f.tree, 100000, 100000 + kTxnKeys)
           (left=0, right=200)
    [FAIL] WriteAbort.RollsBackAllocatorState
           EXPECT_EQ failed: next_before == f.pool->meta().next_unallocated
           (left=14, right=20)
    ========== 1 passed, 3 failed ==========

`CommitStillApplies` stayed green, confirming the experiment moved only the
abort axis and did not simply break every write.

Reverted; `src/storage/buffer_pool.cc` is byte-identical to its pre-experiment
state (`cmp -s` reports identical, sha256
`57a701e54b21a5698dddb3f19975158aa2592b2687f310f0a15f4b02814bbb01`, and zero
`FALSIFICATION EXPERIMENT` markers remain in the tree). Test green again: 4
passed, 0 failed.

**Why each case is a real judgement and not a counter check**

* `RollsBackMemoryPages` counts keys the tree can still find after the abort, and
  it also asserts the keys were findable *before* the abort. Without that second
  assertion the case would pass vacuously if the inserts had silently done nothing.
  It additionally asserts the base segment survives (a rollback that undoes
  everything is not a rollback) and that `Validate()` passes — which is the point:
  the aborted tree is structurally *sound*, just wrong. No invariant fires. Only
  comparison against the pre-transaction state catches it.
* `AbortedStateDoesNotSurviveReopen` reopens the tree from the page file, so it
  cannot be satisfied by a memory-only fix.
* `RollsBackAllocatorState` asserts the allocation cursor moved *during* the
  transaction before asserting it came back — otherwise it would pass on a
  workload that never allocated anything.
* `CommitStillApplies` is the negative control: it fails if the rollback is
  over-applied.

================================================================================
WHAT IS STILL NOT PROVEN
================================================================================
* **C1 is still unimplemented.** This was its precondition, nothing more. The
  C0 read-scaling criterion is still unreachable for the reasons recorded in
  `docs/status.md` §M6-1.
* **No concurrency here.** These cases are single-threaded; the abort path is
  exercised under the exclusive latch's protection. Whether the participant
  callbacks and the rollback remain correct once C1 allows concurrent writers is
  unmeasured — in particular the rollback's two-pass order (resident pages first,
  then pages that were evicted mid-transaction) exists to avoid flushing
  un-rolled-back dirty frames, and that reasoning assumes nobody else is writing.
* **`RTree` upsert was exercised only indirectly**, through the B+ tree in these
  cases. The `HybridIndex::Put` failure path that motivates the whole gate is
  described in `docs/status.md` but not yet driven end-to-end by a test: forcing
  the second tree to fail needs a fault point that is compiled off in every build
  directory here, and routing a negative test through `FaultInjector` would pass
  vacuously (see defect #37).
