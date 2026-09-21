# Cut write amplification in a pinned LSM-tree engine

Optimize a pinned LSM-tree storage engine across a large heterogeneous workload corpus to substantially reduce geometric-mean write amplification while preserving read/write correctness, snapshot and iterator semantics, crash recovery, and on-disk compatibility.

The optimization may require coordinated changes across the write path, memtable flushing, SSTable construction, compaction scheduling, compaction selection, level management, tombstone handling, background I/O, and version/manifest management.

The candidate must pass the complete conformance and recovery suite. Performance is evaluated against a pinned baseline and a declared expert reference score.

## Where things are

`/app/leveldb` is LevelDB 1.23 (`99b3c03b`) with a `pristine` git tag; `/opt/pristine` is an untouched copy. Only `/app/leveldb` is collected. `/app/bench` holds the harness the verifier runs, its README (the twelve workloads, the metric) and `bench.py`:

    python3 /app/bench/bench.py --quick   # 4 workloads, scale 1 (~2 min)
    python3 /app/bench/bench.py           # all twelve at scale 3 (~15 min)
    python3 /app/bench/bench.py --suite   # conformance + recovery slice (~2 min)

## Score

Write amplification (WA) = bytes the engine passes to `write(2)` during a workload / its logical bytes (key+value of every put, key of every delete), from the kernel's I/O counters. Per workload, ratio = your WA / the pristine engine's WA, both measured in the same verifier run on a fixed seed you are not given. **Score = geometric mean of the twelve ratios**, lower is better; untouched is 1.0. `bench.py` estimates it against the pristine's numbers on that seed (`/app/bench/scored_baseline.json`). The expert reference scores REFSCORE. Nothing counts unless every gate below passes; the untouched tree passes them all.

## Gates

1. Builds with the tree's CMake (`-DLEVELDB_BUILD_TESTS=OFF`); the verifier's harness compiles against `include/`.
2. Conformance: 2400 seeded scenarios of puts, deletes, batches, range-clearing batches, point reads, iterator walks mixing Seek/Next/Prev, full scans both ways, snapshots, reopens and compactions, every read checked against a model. All must pass.
3. Recovery: 160 seeded crashes at random Env events (WAL, table, MANIFEST, file removal, rename). Both engines must recover the same acknowledged state from what the candidate left, and the candidate must recover what the pristine left. Plus eleven fixed crash points in a larger workload.
4. Compatibility: the pristine engine reads the candidate's databases and vice versa; six pristine-written databases (clean, live WAL, torn WAL, `reuse_logs`, filterless tables, one fresh) are read and extended by the candidate.
5. Guardrails per workload, vs the pristine: never more than **4 level-0 files overlapping one key**; peak table bytes <= 1.25x; bytes on disk after the run <= 1.5x; phase-F reads <= 1.5x (2.5x on `ttl`); peak files <= 3x; no table over 8 MB; RSS <= +96 MB; memtable switches >= 0.8x; time <= 4x + 60 s.

## Constraints

* Keep the public API (`include/leveldb`) source-compatible and the on-disk format unchanged; all I/O goes through the `Env` the engine is given.
* The verifier uses its own copy of the harness and its own seeds; changing `/app/bench` has no effect.
