# Benchmark harness

Byte copies of what the verifier runs, plus `bench.py` and the scored seed's
baseline.

| file | role |
|---|---|
| `lsmbench.cc` | the harness: `describe`, `run` (write phases), `read` (read-only phase), `check` (verify a database against the model, optionally continue), `selftest`; every mode takes `--workload` |
| `workload.h` | the workload generators and the in-memory model every read is checked against |
| `bench_env.h` | the `leveldb::Env` the harness hands to the engine: POSIX files, byte counters, a deterministic single-worker scheduler drained after every operation, crash injection |
| `manifest.py` | parser of CURRENT/MANIFEST: replays the version edits, reports the level layout over time (level-0 depth, peak space, file sizes, memtable switches) |
| `measure.py` | runs a harness process and reads the kernel's I/O counters of it |
| `bench.py` | builds `/opt/pristine` and `/app/leveldb`, runs every workload with both, prints the guardrails and the estimated score |
| `scored_baseline.json` | the pristine engine's numbers per workload on the verifier's scored seed |

    python3 /app/bench/bench.py                        # all four workloads, seed 1, scale 3 (~4 min)
    python3 /app/bench/bench.py --workload uniform     # one workload (repeatable)
    python3 /app/bench/bench.py --candidate-only       # skip the pristine, compare with scored_baseline.json
    python3 /app/bench/bench.py --scale 1              # quicker, but not comparable to the score

## Workloads (scale 3, the verifier's scale)

| workload | logical MB | what it does | pristine WA on the scored seed |
|---|---|---|---|
| `mixed` | 730 | sequential load; random-order inserts; updates, 75% in one hot key range; deletes, 60% purging a contiguous range in key order; a mixed phase whose hot window slides across the keys; gets, scans, snapshots throughout | 14.10 |
| `uniform` | 556 | load 600k keys, then 1.8M operations: 95% overwrites of uniformly random keys, 5% deletes; gets, scans, snapshots | 10.44 |
| `series` | 598 | ids grow like timestamps, 8 per batch; 12% of writes are late corrections to the last 60k ids; the oldest ids are deleted in key order to keep 540k live; gets and scans of recent data | 4.33 |
| `blob` | 632 | values of 1-6 KB; load 90k keys, then updates of which 70% hit a hot set of 8% of the keys scattered over the key space, 5% deletes; gets and short scans | 5.00 |

Each workload ends with a read-only phase in a separate process (point
reads, forward and reverse scans, one full scan).  Its bytes read are the
phase-F guardrail.  In `series` many scans start inside the purged range,
so how quickly deleted ranges are compacted away shows up there.

## The metric

Per workload, write amplification = bytes passed to `write(2)` by the
harness process (everything the engine writes: WAL, tables, MANIFEST,
CURRENT) for the write phases plus the read-only process, divided by the
logical bytes of the workload (key + value of every put, key of every
delete).  The kernel's counters are read from the stopped process, so
nothing the engine reports is used.

Ratio = candidate WA / pristine WA, and the score is the geometric mean of
the four ratios (untouched = 1.0, lower is better).  Guardrails are checked
per workload; one failure anywhere and the score does not count.

The workloads, options and metric are the same in the verifier; only the
seed differs.  The pristine's WA moves with the seed (16.5 on seed 1 vs
14.10 on the scored seed for `mixed`), and so does a candidate's when it
still relies on seek-triggered compactions.  `bench.py` therefore prints
the ratio against `scored_baseline.json` as the estimate and checks each
ratio guardrail against both baselines, showing the worse.  Expect the
estimate to be within a few percent of the verifier.
