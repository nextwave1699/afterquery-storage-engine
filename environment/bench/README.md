# Benchmark harness

Byte copies of what the verifier runs, plus `bench.py`.

| file | role |
|---|---|
| `lsmbench.cc` | the harness: `describe`, `run` (phases A-E), `read` (phase F), `check` (verify a database against the model, optionally continue), `selftest` |
| `workload.h` | the fixed workload generator and the in-memory model every read is checked against |
| `bench_env.h` | the `leveldb::Env` the harness hands to the engine: POSIX files, byte counters, a deterministic single-worker scheduler drained after every operation, crash injection |
| `manifest.py` | trusted parser of CURRENT/MANIFEST: replays the version edits, reports the level layout over time (level-0 depth, peak space, file sizes, memtable switches) |
| `measure.py` | runs a harness process and reads the kernel's I/O counters of it |
| `bench.py` | builds `/opt/pristine` and `/app/leveldb`, runs the workload with both, prints write amplification and the guardrails |

    python3 /app/bench/bench.py                 # seed 1, scale 3 (verifier scale)
    python3 /app/bench/bench.py --scale 1       # quicker turnaround
    python3 /app/bench/bench.py --candidate-only --keep

Write amplification = bytes passed to `write(2)` by the harness process
(everything the engine writes: WAL, tables, MANIFEST, CURRENT) for the run
of phases A-E plus the phase-F process, divided by the logical bytes of
the workload (key + value of every put, key of every delete).  The kernel's
counters are read from the stopped process, so nothing the engine reports
is used.

The score is the candidate's write amplification divided by the
pristine's (1.0 = untouched, lower is better).  The workload, options and
metric are the same in the verifier; only the seed differs (the verifier
uses a fixed seed of its own; results are seed-stable to about 1%).
