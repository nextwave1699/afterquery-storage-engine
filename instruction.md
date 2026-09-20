# Cut LevelDB's write amplification across twelve workloads

`/app/leveldb` is LevelDB 1.23 (`99b3c03b`), with a `pristine` git tag; `/opt/pristine` is an untouched copy. `/app/bench` holds the harness the verifier runs, its README and `bench.py`:

    python3 /app/bench/bench.py --quick   # 4 workloads, scale 1 (~2 min)
    python3 /app/bench/bench.py           # the real measurement (~15 min)

## Goal

Change the engine's compaction strategy so the twelve workloads are written with far fewer bytes, while reads, crash recovery and the on-disk format stay as they are.

**Write amplification** (WA) = bytes the engine writes during a workload (WAL, tables, MANIFEST, everything) / its logical bytes (key+value of every put, key of every delete), from the kernel's I/O counters. Per workload, ratio = your WA / the pristine engine's WA, both measured in the same verifier run on a fixed seed you are not given. **Score = geometric mean of the twelve ratios**; untouched is 1.0, lower is better, and it only counts if every gate below holds on every workload; the untouched tree passes them.

`bench.py` estimates your ratios against the pristine's numbers on the scored seed (`/app/bench/scored_baseline.json`), to within a few percent.

The workloads pull in different directions: disabling seek compactions cuts `mixed` and `bursts` a lot but leaves `series` and `ttl` scanning tombstones, 20x over the read guardrail. Spread over twelve workloads, that strategy averages 0.69. There is no finish line: results are ranked against other submissions, and a first policy that passes the gates is a starting point. Keep measuring and trying new policies for as long as you have time.

## Workloads

`workload.h`, 250-730 MB of writes each with reads interleaved, then a read-only phase; every read is checked against a model. `/app/bench/README.md` has the full table.

`mixed`, `uniform` (random overwrites), `hotkey` (90% of writes to 1% of keys), `rolling` (overwrite sweeps in key order), `bursts` (write bursts, then read-only lulls), `smallval` (16-48 B values), `blob` (1-6 KB values, scattered hot set), `wide` (sparse writes over a key space 4x the data), `scan` (15% long scans), `bimodal` (tail appends interleaved with updates low in the key space), `series` and `ttl` (appends with late corrections, oldest ids purged in key order; `ttl` keeps a tenth live).

Fixed options: 4 MB write buffer, 2 MB target file size, 4 KB blocks, 8 MB block cache, 10-bit bloom filter, no compression.

## Gates

1. Builds with the tree's CMake (`-DLEVELDB_BUILD_TESTS=OFF`); the harness compiles against `include/`.
2. Small `mixed` run: candidate writes, pristine reads; pristine writes, candidate reads and extends.
3. Crash recovery: SIGKILL at eleven points (WAL append, table close, either side of a MANIFEST append, file removal, mid-table, mid-compaction, reopen, torn WAL tail). Acknowledged batches must be recovered by the candidate and by the pristine on the same directory; the candidate continues; the pristine reads the result; no missing, unreferenced or stale files remain.
4. Compatibility: six pristine-written databases (clean, live WAL, torn WAL, `reuse_logs`, filterless 16 KB-block tables, one fresh) are read and extended by the candidate, then read by the pristine.
5. Guardrails per workload, vs the pristine: never more than **4 level-0 files overlapping one key**; peak table bytes <= 1.25x; bytes on disk after the run <= 1.5x; phase-F reads <= 1.5x (2.5x on `ttl`); peak files <= 3x; no table over 8 MB; RSS <= +96 MB; memtable switches >= 0.8x; time <= 4x + 60 s. Each engine reads back the other's database.

## Constraints

* Keep the public API (`include/leveldb`) source-compatible and the on-disk format unchanged; all I/O goes through the `Env` the engine is given.
* Only `/app/leveldb` is collected; the verifier uses its own copy of the harness.
