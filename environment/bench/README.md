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

    python3 /app/bench/bench.py --quick                # fast proxy (~2 min)
    python3 /app/bench/bench.py                        # all twelve (~15 min)
    python3 /app/bench/bench.py --workload uniform     # one workload (repeatable)
    python3 /app/bench/bench.py --candidate-only       # skip the pristine build

`--quick` runs mixed, bursts, ttl and bimodal at scale 1: four different
pressures (write volume, flush behaviour, tombstone scans, flushes that span
the key range) in about two minutes.  It is a proxy, not the score: fewer
workloads, and a smaller scale builds fewer levels, so the ratios come out
higher than at scale 3.  Use it to throw ideas away, then confirm the
survivors with a full run.

## Workloads (scale 3, the verifier's scale)

All twelve are scored.  `mixed` is also the workload the crash-recovery,
compatibility and differential stages use.

| workload | logical MB | pristine WA | what it does |
|---|---|---|---|
| `mixed` | 730 | 14.10 | sequential load; random-order inserts; updates 75% in one hot range; deletes, 60% purging a contiguous range in key order; a mixed phase whose hot window slides across the keys; gets, scans, snapshots |
| `uniform` | 556 | 10.44 | load 600k keys, then 1.8M writes: 95% overwrites of uniformly random keys, 5% deletes; gets, scans, snapshots |
| `hotkey` | 564 | 3.54 | load 600k keys, then 1.8M writes with 90% of them into a hot set of 1% of the keys, scattered over the key space; snapshots |
| `rolling` | 640 | 7.23 | load 900k keys, then 1.8M writes by a cursor sweeping the key space in order, over and over |
| `bursts` | 487 | 15.43 | load 600k keys, then bursts of 200 write batches (up to 12 writes each) alternating with read-only lulls |
| `smallval` | 253 | 9.72 | values of 16-48 bytes: load 1.8M keys, then 3.6M writes in batches of up to 16 |
| `blob` | 632 | 5.13 | values of 1-6 KB: load 90k keys, then updates, 70% of them into a hot set of 8% of the keys |
| `wide` | 319 | 9.55 | sparse writes over a key space four times the data: load 150k keys, then 1.2M writes to random ids |
| `scan` | 424 | 8.00 | load 900k keys, then 900k writes with 15% of all steps a range scan of 500-2000 keys |
| `bimodal` | 528 | 5.08 | load 450k keys, then 1.8M writes: half extend the tail with new ids, half update a range low in the key space, so one flushed memtable spans nearly the whole key range |
| `series` | 598 | 4.33 | ids grow like timestamps, 8 per batch; 12% of writes are late corrections to the last 60k ids; the oldest ids are deleted in key order to keep 540k live; gets and scans of recent data |
| `ttl` | 495 | 3.74 | like series but a tenth as much stays live (180k of 1.8M ids) and scans are longer, so phase-F scans cross long runs of tombstones |

Each workload ends with a read-only phase in a separate process (point
reads, forward and reverse scans, one full scan).  Its bytes read are the
phase-F guardrail: <= 1.5x the pristine engine, except `ttl`, where a tenth
of the ids stay live, scans cross long tombstone runs and read cost swings
much harder, so the limit is 2.5x.

## The metric

Per workload, write amplification = bytes passed to `write(2)` by the
harness process (everything the engine writes: WAL, tables, MANIFEST,
CURRENT) for the write phases plus the read-only process, divided by the
logical bytes of the workload (key + value of every put, key of every
delete).  The kernel's counters are read from the stopped process, so
nothing the engine reports is used.

Ratio = candidate WA / pristine WA, and the score is the geometric mean of
the twelve ratios (untouched = 1.0, lower is better).  Guardrails are checked
per workload; one failure anywhere and the score does not count.

The workloads, options and metric are the same in the verifier; only the
seed differs.  The pristine's WA moves with the seed (16.5 on seed 1 vs
14.10 on the scored seed for `mixed`), and so does a candidate's when it
still relies on seek-triggered compactions.  `bench.py` therefore prints
the ratio against `scored_baseline.json` as the estimate and checks each
ratio guardrail against both baselines, showing the worse.  Expect the
estimate to be within a few percent of the verifier.
