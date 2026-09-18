# storage-engine

Change the compaction strategy of a pinned LevelDB (1.23, commit `99b3c03b`)
so that a fixed 730 MB LSM-tree workload is written with as few bytes as
possible, while every read stays correct, crash recovery works at every
engine event for both the candidate and the pristine engine, the on-disk
format stays byte-compatible in both directions, and level-0 depth, space,
read bytes and memory stay inside the baseline's envelope.  The agent's
brief is `instruction.md`.

Paradigm: performance.  The gate (`gate_passed`, = `reward.txt`) is every
correctness stage plus the guardrails; the untouched tree passes it.  The
objective is `write_amp_ratio` = candidate write amplification / pristine
write amplification, both measured in the same verifier run on a fixed
sealed seed (`BENCH_SEED` in `tests/verify.py`).  Lower is better; the
untouched tree scores exactly 1.0 (WA 14.10), the reference 0.350 (WA 4.93).

## Layout

| path | role |
|---|---|
| `environment/` | agent image: toolchain, `/app/leveldb` (pristine tree with vendored googletest/benchmark, git tag `pristine`), `/opt/pristine` (untouched copy), `/app/bench` (byte copies of the harness + `bench.py`) |
| `environment/sealed/leveldb.tar.gz` | the pinned source, built by `scripts/seal.sh` |
| `tests/` | verifier image, `test.sh`, `verify.py` (driver), `test_outputs.py` (verdict), `harness/` (the harness; `environment/bench` is a copy), `sealed/` (source + fixtures) |
| `solution/` | reference solution: `compaction.patch` + `solve.sh` |
| `cheat/README.md` | shortcuts considered and why the verifier rejects them (never executed by the pipeline) |
| `scripts/` | `seal.sh` (rebuild the archives and fixtures), `gen_fixtures.sh`, `docker_failure_paths.sh` (17 trees through the verifier image) |
| `task.toml` | contract: gate `gate_passed`, objective `write_amp_ratio` (lower, baseline 1.0) |

## The harness

`tests/harness/lsmbench.cc` links against either engine and supplies its own
`leveldb::Env` (`bench_env.h`): POSIX files with byte counters, a
single-worker scheduler for `Env::Schedule` that the harness drains after
every operation (compactions happen at deterministic points, so byte counts
do not depend on timing), and SIGKILL injection at Env events.  The
workload (`workload.h`) is a pure function of (seed, scale); an in-memory
model of one 32-bit version per key regenerates the value expected from any
read, so every point read, scan, reverse scan, snapshot read and the final
full scan are checked.  `lsmbench check --batches B` verifies that a
database holds exactly the state after B (or B+1, an in-flight batch)
write batches, and `--continue K` extends it.

Write amplification = `wchar` from `/proc/<pid>/io` of the run process
(phases A-E) plus the phase-F process, divided by the logical bytes computed
by the pristine harness.  The harness stops itself (SIGSTOP) when done; the
measurement helper reads the kernel counters of the stopped process and
kills it, so nothing after `main()` can add or hide I/O.  `manifest.py`
replays the MANIFEST (CURRENT + log-format VersionEdits) without the engine
and yields the level layout after every version: level-0 depth, peak table
bytes, file count and size, memtable switches.

## Verification

`tests/test.sh` -> `verify.py` (root; candidate code runs as user `lsm`):

1. build the pristine engine and harness; build the candidate as `lsm`;
   compile the verifier's own harness against the candidate's `include/`;
2. selftest through the public API;
3. differential at scale 0.25: candidate writes / pristine reads, pristine
   writes / candidate reads and extends / pristine reads;
4. eleven crash scenarios at scale 0.3 (WAL append, table close, before and
   after MANIFEST appends, before an obsolete-file removal, mid-table,
   mid-compaction, after a compaction is installed, reopen before the
   CURRENT rename, crash during recovery, torn WAL tail): acknowledged
   batches recovered by the candidate and by the pristine engine on the
   same directory, candidate continues, pristine reads, candidate reopens,
   directory consistent (no missing tables, bounded leftovers);
5. compatibility: five sealed pristine-written fixtures (clean, live WAL,
   torn WAL, `reuse_logs`, filterless 16 KB-block tables) and one generated
   at verify time; candidate reads and extends, pristine reads;
6. the full workload (scale 3, fixed `BENCH_SEED`) with both engines,
   cross reads at full scale, guardrails, and the score.

`reward.txt` and `gate_passed` are 1 when every stage and every guardrail
hold.  `write_amp_ratio` is reported as 1.0 whenever the gate fails.  Both
reward files are written with reward 0 before anything runs and rewritten
at the end.  The random seeds only drive the unscored correctness stages.

## Baseline (nop) and reference, verifier seed

Measured with the full `tests/test.sh` (verify.py + pytest + reward files) on
Ubuntu 24.04 / g++ 13.3, LevelDB compiled with the Release flags:

| run | gate | pristine WA | candidate WA | `write_amp_ratio` |
|---|---|---|---|---|
| nop, run 1 | 1 | 14.0985 | 14.0985 | 1.0000 |
| nop, run 2 (other correctness seed) | 1 | 14.0985 | 14.0985 | 1.0000 |
| seek-only (one-line change) | 1 | 14.0985 | 6.069 | 0.430 |
| oracle (`solution/`) | 1 | 14.0985 | 4.934 | 0.350 |

The ratio of the untouched tree is 1.0 by construction (same source, same
seed; the harness drains the background queue after every operation, so the
byte counts do not depend on timing), and both runs gave bit-identical
write amplification.  Oracle guardrails: level-0 depth 4, peak space
1.15x, phase-F reads 1.00x, peak files 1.65x, wall time 0.56x.

## Calibration (scale 3, seeds 1-3, level-0 depth <= 4, older contract)

| engine | write amplification |
|---|---|
| pristine | 14.8 - 16.5 (seek-compaction volume varies with the seed) |
| no seek-triggered compactions (one line) | 6.06 - 6.09 |
| + min-overlap file selection | 5.65 - 5.72 |
| no seek + grandparent-aligned outputs | 5.67 |
| no seek + min-overlap + L1 = 30 MB | 6.07 |
| reference (no seek, cheapest cold file, aligned outputs) | 4.97 - 5.01 |
| constants only: no seek, L0 trigger 12, L1 = 40 MB | 4.70, but level-0 depth 12 (rejected) |

The no-seek variants are seed-stable to about 1%; the pristine number moves
more with the seed (seek-compaction volume), which is why the scored seed is
fixed.  The pristine measurement is taken live and must fall in [11, 22].

## Provenance

* Reference solution (`solution/compaction.patch`, ~300-line diff to
  `db/version_set.{h,cc}`, `db/db_impl.cc`, `db/version_edit.h`): reads no
  longer schedule compactions; size compactions at level >= 1 pick the file
  with the smallest next-level overlap per byte, scaled by an in-memory
  "hotness" (fresh fraction of each output, from sequence numbers); outputs
  are cut at grandparent file boundaries.  WA 4.93 on the verifier's seed
  (ratio 0.350) with all guardrails inside the envelope.
* Earlier Harbor runs used the old contract (a fixed 5.40 bar inside the
  gate, random benchmark seed): `docker_failure_paths.sh` with every broken
  or cheating tree at 0, `harbor run -a oracle` (1, WA 4.98), `harbor run
  -a nop` (twice, pristine 14.3 and 16.7).  That is why the benchmark seed
  is now fixed.  The table above is the current contract.  `harbor check`
  was not run (no API credentials on the build machine).
