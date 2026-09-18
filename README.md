# storage-engine

Change the compaction strategy of a pinned LevelDB (1.23, commit `99b3c03b`)
so that a fixed 730 MB LSM-tree workload is written with a kernel-measured
write amplification of at most **5.40** (the untouched engine measures
15-16.5), while every read stays correct, crash recovery works at every
engine event for both the candidate and the pristine engine, the on-disk
format stays byte-compatible in both directions, and level-0 depth, space,
read bytes and memory stay inside the baseline's envelope.  The agent's
brief is `instruction.md`.

## Layout

| path | role |
|---|---|
| `environment/` | agent image: toolchain, `/app/leveldb` (pristine tree with vendored googletest/benchmark, git tag `pristine`), `/opt/pristine` (untouched copy), `/app/bench` (byte copies of the harness + `bench.py`) |
| `environment/sealed/leveldb.tar.gz` | the pinned source, built by `scripts/seal.sh` |
| `tests/` | verifier image, `test.sh`, `verify.py` (driver), `test_outputs.py` (verdict), `harness/` (the harness; `environment/bench` is a copy), `sealed/` (source + fixtures) |
| `solution/` | reference solution: `compaction.patch` + `solve.sh` |
| `cheat/README.md` | shortcuts considered and why the verifier rejects them (never executed by the pipeline) |
| `scripts/` | `seal.sh` (rebuild the archives and fixtures), `gen_fixtures.sh`, `docker_failure_paths.sh` (17 trees through the verifier image) |
| `task.toml` | contract: gate `gate_passed`, objective `scored_write_amp` |

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
6. the full workload (scale 3, seed drawn at verify time) with both engines,
   cross reads at full scale, guardrails, the bar.

`reward.txt` is 1 only when every stage, every guardrail and
`write_amp <= 5.40` hold.  Both reward files are written fail-closed before
anything runs and rewritten at the end.

## Calibration (scale 3, seeds 1-3, level-0 depth <= 4)

| engine | write amplification |
|---|---|
| pristine | 14.8 - 16.5 (seek-compaction volume varies with the seed) |
| no seek-triggered compactions (one line) | 6.06 - 6.09 |
| + min-overlap file selection | 5.65 - 5.72 |
| no seek + grandparent-aligned outputs | 5.67 |
| no seek + min-overlap + L1 = 30 MB | 6.07 |
| reference (no seek, cheapest cold file, aligned outputs) | 4.97 - 5.01 |
| constants only: no seek, L0 trigger 12, L1 = 40 MB | 4.70, but level-0 depth 12 (rejected) |

The no-seek variants are seed-stable to about 1%, which is why the bar is
absolute rather than a ratio to the (noisier) pristine number; the pristine
measurement is still taken live and must fall in [11, 22].

## Provenance

* Reference solution (`solution/compaction.patch`, ~300-line diff to
  `db/version_set.{h,cc}`, `db/db_impl.cc`, `db/version_edit.h`): reads no
  longer schedule compactions; size compactions at level >= 1 pick the file
  with the smallest next-level overlap per byte, scaled by an in-memory
  "hotness" (fresh fraction of each output, from sequence numbers); outputs
  are cut at grandparent file boundaries.  Measures 4.98-5.01 on the
  verifier's workload with all guardrails inside the envelope.
* Validated on the Harbor execution server: `docker_failure_paths.sh`
  (nop 0, oracle 1, fifteen broken/cheating/insufficient trees all 0 with
  reward files present), `harbor run -a oracle` (1, WA 4.98), `harbor run
  -a nop` (0, twice: baselines 14.3 and 16.7).  `harbor check` was not run
  (no API credentials on the build machine).
