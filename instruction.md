# Cut LevelDB's write amplification across four workloads

`/app/leveldb` is LevelDB 1.23 (commit `99b3c03b`), a git repo whose `pristine` tag is the untouched code; `/opt/pristine` is an untouched copy. `/app/bench` holds the harness the verifier runs, its README and `bench.py`, which builds both trees and measures them:

    python3 /app/bench/bench.py                     # all workloads, scale 3 (~4 min)
    python3 /app/bench/bench.py --workload series   # just one

## Goal

Change the engine's compaction strategy so that four fixed workloads are written with far fewer bytes, while reads, crash recovery and the on-disk format stay exactly as they are.

**Write amplification** (WA) = bytes the engine passes to `write(2)` (WAL, tables, MANIFEST, everything) during a workload / its logical bytes (key+value of every put, key of every delete), from the kernel's I/O counters. Per workload, ratio = your WA / the pristine engine's WA, both measured in the same verifier run on a fixed seed you are not given. **Score = geometric mean of the four ratios**; untouched is 1.0, lower is better, and it only counts if every gate below holds on every workload (the untouched tree passes). There is no target: submissions are ranked by score.

`/app/bench/scored_baseline.json` has the pristine's numbers on the scored seed; `bench.py` estimates your ratios against them (within a few percent) and checks guardrails against both baselines.

The workloads pull in different directions (disabling seek compactions cuts `mixed` a lot but makes `series` scans read 24x more). Expect many hours of measuring and iterating.

## Workloads

`workload.h`, 550-730 MB of writes each with reads interleaved, then a read-only phase in a separate process; every read is checked against an in-memory model.

* `mixed`: load, random inserts, hot-range updates, a range purge plus random deletes, a sliding hot window, snapshots.
* `uniform`: load, then uniform random overwrites, a few deletes, snapshots.
* `series`: time-ordered appends, 12% late corrections to recent keys, oldest keys purged in key order.
* `blob`: 1-6 KB values; 70% of updates hit a scattered 8% hot set.

Fixed options: 4 MB write buffer, 2 MB target file size, 4 KB blocks, 8 MB block cache, 10-bit bloom filter, no compression. The harness's `Env` counts bytes, drains background work after every operation (deterministic compaction points) and injects crashes at Env events.

## Gates

1. Builds with the tree's CMake (`-DLEVELDB_BUILD_TESTS=OFF`); the harness compiles against `include/`.
2. Small `mixed` run: candidate writes, pristine reads; pristine writes, candidate reads and extends.
3. Crash recovery: SIGKILL at WAL append, table close, before/after a MANIFEST append, before a file removal, mid-table, mid-compaction, during reopen and a torn WAL tail. Acknowledged batches must be recovered (the in-flight one may or may not be) by the candidate and by the pristine on the same directory; the candidate continues; the pristine reads the result; no missing, unreferenced or stale files remain.
4. Compatibility: sealed pristine-written databases (clean, live WAL, torn WAL, `reuse_logs`, filterless 16 KB-block tables) and a fresh one are read and extended by the candidate, then read by the pristine.
5. Guardrails per workload, vs the pristine: at no version more than **4 level-0 files overlapping one key**; peak table bytes <= 1.25x; table bytes on disk after the run <= 1.5x; phase-F bytes read <= 1.5x; peak file count <= 3x; no table over 8 MB; peak RSS <= +96 MB; memtable switches >= 0.8x; wall time <= 4x + 60 s. Each engine reads back the other's database.

## Constraints

* Keep the public API (`include/leveldb`) source-compatible and the on-disk format unchanged.
* All I/O must go through the `Env` the engine is given.
* Only `/app/leveldb` is collected; the verifier uses its own copy of the harness.
