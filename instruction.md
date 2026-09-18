# Cut LevelDB's write amplification on a fixed workload

`/app/leveldb` is LevelDB 1.23 (commit `99b3c03b`), a git repo whose `pristine` tag is the untouched code; `/opt/pristine` is an untouched copy. `/app/bench` holds the harness the verifier runs (`lsmbench.cc`, `workload.h`, `bench_env.h`, `manifest.py`) and `bench.py`, which builds both trees and measures them:

    python3 /app/bench/bench.py     # seed 1, scale 3 (~1 min): score and gates

## Goal

Change the engine's compaction strategy so that the fixed workload is written with far fewer bytes, while reads, crash recovery and the on-disk format stay exactly as they are.

**Write amplification** (WA) = bytes the engine passes to `write(2)` (WAL, tables, MANIFEST, everything) during the workload / logical bytes (key+value of every put, key of every delete), from the kernel's I/O counters. Score = your WA / the pristine engine's WA, both measured in the same verifier run on a fixed seed you are not given. Lower is better, untouched is 1.0, and it only counts if every gate below passes (the untouched tree passes them).

On the scored seed the pristine WA is 14.10 (`/app/bench/scored_baseline.json`). Your WA barely moves with the seed, so `bench.py` estimates the score as your WA / 14.10 and checks guardrails against that baseline; the seed-1 ratio looks better than you will score.

For scale: disabling seek-triggered compactions alone scores about 0.43; the reference solution scores 0.35 (WA 4.93). Neither is a ceiling. Expect many hours of measuring and iterating, not one patch.

## Workload

`workload.h`: ~730 MB of writes in phases (sequential load; random inserts; hot-range updates; a range purge in key order plus random deletes; a mixed phase with a hot window sliding across the keys), with point reads, scans and snapshot reads interleaved, then a read-only phase in a separate process. Every read is checked against an in-memory model. Fixed options: 4 MB write buffer, 2 MB target file size, 4 KB blocks, 8 MB block cache, 10-bit bloom filter, no compression.

The harness's `Env` counts bytes, drains background work after every operation (so compactions happen at deterministic points) and injects crashes at Env events.

## Gates

1. Builds with the tree's CMake (`-DLEVELDB_BUILD_TESTS=OFF`); the harness compiles against `include/`.
2. Small workload: candidate writes, pristine reads; pristine writes, candidate reads and extends.
3. Crash recovery: SIGKILL at WAL append, table close, before/after a MANIFEST append, before a file removal, mid-table, mid-compaction, during reopen (before the CURRENT rename, after the recovery MANIFEST) and a torn WAL tail. Acknowledged batches must be recovered (the in-flight one may or may not be) by the candidate and by the pristine engine on the same directory; the candidate continues; the pristine reads the result; no missing, unreferenced or stale files remain.
4. Compatibility: sealed pristine-written databases (clean close, live WAL, torn WAL, `reuse_logs`, filterless 16 KB-block tables) and a fresh one are read and extended by the candidate, then read by the pristine.
5. Guardrails on the full workload, from the MANIFEST history and the kernel: at no version more than **4 level-0 files overlapping one key**; peak table bytes <= 1.25x baseline; table bytes on disk after the run <= 1.5x; phase-F bytes read <= 1.5x; peak file count <= 3x; no table over 8 MB; peak RSS <= baseline + 96 MB; at least 0.8x the baseline's memtable switches; wall time <= 4x baseline + 60 s.

## Constraints

* Keep the public API (`include/leveldb`) source-compatible and the on-disk format unchanged: the pristine engine must read what you write and vice versa.
* All I/O must go through the `Env` the engine is given; compaction must actually run.
* Only `/app/leveldb` is collected. The verifier uses its own copies of the harness, so changing `/app/bench` has no effect.
