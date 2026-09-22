# Harness

Byte copies of what the verifier compiles and runs, plus `bench.py`.  The
contract these check is `/app/spec.md`.

| file | role |
|---|---|
| `conftest.cc`, `conf_model.h`, `conf_model.cc` | the conformance suite: seeded scenarios checked against a model (`scenarios`, `--ext` for range deletes and merges), crash and recovery (`crash`, `recover`), the API checks (`api`), the efficiency cases (`perf`) |
| `bench_env.h` | the `leveldb::Env` the harness hands to the engine: POSIX files, byte counters, a deterministic single-worker scheduler drained after every operation, crash injection at Env events |
| `lsmbench.cc`, `workload.h` | the stock-workload harness: `run`, `read`, `check`, `describe`, `selftest` (the untouched API only) |
| `manifest.py` | parser of CURRENT/MANIFEST: replays the version edits, reports the level layout over time |
| `measure.py` | runs a harness process and reads the kernel's I/O counters of it |
| `bench.py` | the self-check: builds both engines and the harness, runs everything the verifier runs on other seeds |

    python3 /app/bench/bench.py            # everything
    python3 /app/bench/bench.py --suite    # API checks, scenarios, crash recovery
    python3 /app/bench/bench.py --perf     # the efficiency cases
    python3 /app/bench/bench.py --bench    # the stock regression benchmark
    python3 /app/bench/bench.py --quick    # a tenth of the suite, benchmark at scale 1

`conftest` can be run by hand once `bench.py` has built it
(`/tmp/lsm-bench/conftest-cand`):

    conftest-cand scenarios --db /tmp/x --from 7 --to 8 --ext            # one scenario
    conftest-cand scenarios --db /tmp/x --from 7 --to 8 --ext --ops 3000 # a long one
    conftest-cand crash --db /tmp/x --seed 7 --crash log_append:20 --ack /tmp/ack --ext
    conftest-cand recover --db /tmp/x --seed 7 --acked $(cat /tmp/ack) --ext
    conftest-cand perf --db /tmp/x --case rangedel     # or manyranges, merge

A failing scenario prints the operation number and the read that went
wrong.  The operations of a scenario depend only on its seed (see
`ScenarioGen` in `conf_model.cc`), so a failure reproduces exactly.

## Stock workloads (the regression benchmark)

The verifier runs these three on both engines at scale 3 on a sealed seed,
and each engine reads back the other's database.  The candidate may write at
most 1.10x the pristine's bytes, and the guardrails below hold per workload.

| workload | logical MB | pristine WA | what it does |
|---|---|---|---|
| `mixed` | 730 | 14.10 | sequential load; random-order inserts; updates 75% in one hot range; deletes, 60% purging a contiguous range in key order; a mixed phase whose hot window slides across the keys; gets, scans, snapshots |
| `bimodal` | 528 | 5.08 | load 450k keys, then 1.8M writes: half extend the tail with new ids, half update a range low in the key space |
| `ttl` | 495 | 3.74 | ids grow like timestamps; the oldest are deleted in key order so a tenth stays live; long scans of recent data |

Each workload ends with a read-only phase in a separate process (point
reads, forward and reverse scans, one full scan).

Write amplification = bytes passed to `write(2)` by the harness processes
(everything the engine writes: WAL, tables, MANIFEST, CURRENT) divided by
the logical bytes of the workload (key + value of every put, key of every
delete), from the kernel's counters of the stopped process.

Guardrails, candidate vs pristine on the same run: never more than 4
level-0 files overlapping one key; peak table bytes <= 1.25x; table bytes on
disk after the run <= 1.5x; read-phase bytes read <= 1.5x (2.5x on `ttl`);
peak file count <= 3x; no table over 8 MB; peak RSS <= +96 MB; memtable
switches >= 0.8x; wall time <= 4x + 60 s.
