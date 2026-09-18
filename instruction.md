# Cut LevelDB's write amplification on a fixed workload

`/app/leveldb` is LevelDB 1.23 (commit `99b3c03b`, googletest/benchmark vendored), a git repo whose `pristine` tag is the untouched code; `/opt/pristine` is an untouched copy. `/app/bench` holds the benchmark harness the verifier runs (`lsmbench.cc`, `workload.h`, `bench_env.h`, `manifest.py`) and `bench.py`, which builds both trees and measures them:

    python3 /app/bench/bench.py     # seed 1, scale 3 (~1 min): score and gates

## Goal

Change the engine's compaction strategy so that the fixed workload is written with far fewer bytes, while reads, crash recovery and the on-disk format stay exactly as they are.

**Write amplification** = bytes the engine passes to `write(2)` (WAL, tables, MANIFEST, everything) during the workload / logical bytes of the workload (key+value of every put, key of every delete). It is read from the kernel's I/O counters, not from engine reports. Score = your write amplification / the pristine engine's, both measured in the same verifier run: untouched is 1.0 (about 14 WA), lower is better. It only counts if every gate below passes (the untouched tree passes them).

## Workload

`workload.h`: ~730 MB of writes in phases (sequential load; random-order inserts; updates concentrated in one hot key range; deletes purging a contiguous range in key order plus random deletes; a mixed phase with a hot window sliding across the key space), with point reads, range scans and snapshot reads interleaved, then a read-only phase (point reads, forward/reverse scans, full scan) in a separate process. Every read is checked against an in-memory model. Fixed options: 4 MB write buffer, 2 MB target file size, 4 KB blocks, 8 MB block cache, 10-bit bloom filter, no compression. The score uses one fixed seed you are not given; results are seed-stable to ~1%.

The harness supplies its own `leveldb::Env`: it counts bytes, runs `Env::Schedule` work on one worker thread that is drained after every operation (so flushes and compactions happen at deterministic points), and kills the process at chosen Env events for the crash tests.

## Gates

1. Builds with the tree's CMake (`-DLEVELDB_BUILD_TESTS=OFF`); the harness compiles against `include/`.
2. Small workload: candidate writes, pristine reads; pristine writes, candidate reads and extends.
3. Crash recovery: SIGKILL at WAL append, table close, before/after a MANIFEST append, before an obsolete-file removal, mid-table write, mid-compaction, during reopen (before the CURRENT rename, after the recovery MANIFEST) and a torn WAL tail. Acknowledged batches must be recovered (the batch in flight may or may not be), by the candidate and by the pristine engine opening the same directory; the candidate continues writing; the pristine reads the result; no missing, unreferenced or stale files remain.
4. Compatibility: sealed databases written by the pristine engine (clean close, live WAL, torn WAL, `reuse_logs`, tables without filter and with 16 KB blocks) and a fresh one must be read and extended by the candidate, then read by the pristine.
5. Guardrails on the full workload, from the MANIFEST history and the kernel: at no version more than **4 level-0 files overlapping one key**; peak table bytes <= 1.25x baseline; phase-F bytes read <= 1.5x; peak file count <= 3x; no table over 8 MB; peak RSS <= baseline + 96 MB; at least 0.8x the baseline's memtable switches (honour `write_buffer_size`); wall time <= 4x baseline + 60 s.

## Constraints

* Keep the public API (`include/leveldb`) source-compatible and the on-disk format unchanged: the pristine engine must read what you write and vice versa.
* All I/O must go through the `Env` the engine is given; compaction must actually run (level-0 depth, space and read guardrails are checked).
* Only `/app/leveldb` is collected. The verifier uses its own copies of the harness, so changing `/app/bench` has no effect.
