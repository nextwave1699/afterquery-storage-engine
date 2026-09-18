# storage-engine

Change the compaction strategy of a pinned LevelDB (1.23, commit `99b3c03b`)
so that four fixed LSM-tree workloads are written with as few bytes as
possible, while every read stays correct, crash recovery works at every
engine event for both the candidate and the pristine engine, the on-disk
format stays byte-compatible in both directions, and level-0 depth, space,
read bytes and memory stay inside the baseline's envelope on every workload.
The agent's brief is `instruction.md`; `environment/bench/README.md` (in the
image as `/app/bench/README.md`) describes the workloads.

Paradigm: performance, no reference solution (so no solve bar; the score is
open-ended).  The gate (`gate_passed`, = `reward.txt`) is every correctness
stage plus every workload's guardrails; the untouched tree passes it.  The
objective is `write_amp_ratio` = geometric mean over the workloads `mixed`,
`uniform`, `series` and `blob` of candidate write amplification / pristine
write amplification, both measured in the same verifier run on a fixed
sealed seed (`BENCH_SEED` in `tests/verify.py`).  Lower is better; the
untouched tree scores 1.0.

Who would use a solution: anyone running LevelDB (or a fork) on flash, where
bytes written are device wear and background I/O; the changes are the kind
RocksDB made over years (compaction picking, output cutting,
deletion-triggered compaction), done here under strict compatibility.

## Layout

| path | role |
|---|---|
| `environment/` | agent image: toolchain, `/app/leveldb` (pristine tree with vendored googletest/benchmark, git tag `pristine`), `/opt/pristine` (untouched copy), `/app/bench` (byte copies of the harness, `bench.py`, `scored_baseline.json`, README) |
| `environment/sealed/leveldb.tar.gz` | the pinned source, built by `scripts/seal.sh` |
| `tests/` | verifier image, `test.sh`, `verify.py` (driver), `test_outputs.py` (verdict), `harness/` (the harness; `environment/bench` is a copy), `sealed/` (source + fixtures) |
| `cheat/README.md` | shortcuts considered and why the verifier rejects them (never executed by the pipeline) |
| `scripts/` | `seal.sh` (rebuild the archives and fixtures), `gen_fixtures.sh`, `docker_failure_paths.sh` (trees through the verifier image), `sample_candidate.patch` (a known-valid engine), `old_reference.patch` (the earlier single-workload reference, now rejected) |
| `task.toml` | contract: gate `gate_passed`, objective `write_amp_ratio` (lower, baseline 1.0) |

## The harness

`tests/harness/lsmbench.cc` links against either engine and supplies its own
`leveldb::Env` (`bench_env.h`): POSIX files with byte counters, a
single-worker scheduler for `Env::Schedule` that the harness drains after
every operation (compactions happen at deterministic points, so byte counts
do not depend on timing), and SIGKILL injection at Env events.  Every
workload (`workload.h`, `--workload NAME`) is a pure function of (seed,
scale); an in-memory model of one 32-bit version per key regenerates the
value expected from any read, so every point read, scan, reverse scan,
snapshot read and the final full scan are checked.  `lsmbench check
--batches B` verifies that a database holds exactly the state after B (or
B+1, an in-flight batch) write batches, and `--continue K` extends it.

`mixed` is the original workload and byte-for-byte unchanged (the sealed
fixtures and the crash/compat/differential stages use only it); `uniform`,
`series` and `blob` were added for the score.  Value lengths come from a
per-workload profile (48-400 bytes, `blob` 1-6 KB).

Write amplification = `wchar` from `/proc/<pid>/io` of the run process plus
the read-phase process, divided by the logical bytes computed by the
pristine harness.  The harness stops itself (SIGSTOP) when done; the
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
3. differential (`mixed`, scale 0.25): candidate writes / pristine reads,
   pristine writes / candidate reads and extends / pristine reads;
4. eleven crash scenarios (`mixed`, scale 0.3: WAL append, table close,
   before and after MANIFEST appends, before an obsolete-file removal,
   mid-table, mid-compaction, after a compaction is installed, reopen before
   the CURRENT rename, crash during recovery, torn WAL tail): acknowledged
   batches recovered by the candidate and by the pristine engine on the same
   directory, candidate continues, pristine reads, candidate reopens,
   directory consistent (no missing tables, bounded leftovers);
5. compatibility: five sealed pristine-written fixtures (clean, live WAL,
   torn WAL, `reuse_logs`, filterless 16 KB-block tables) and one generated
   at verify time; candidate reads and extends, pristine reads;
6. every workload at scale 3 and `BENCH_SEED` with both engines, cross reads
   at full scale, guardrails per workload, and the score.

`reward.txt` and `gate_passed` are 1 when every stage and every guardrail
hold.  `write_amp_ratio` and the informational `write_amp_ratio_<workload>`
keys are reported as 1.0 whenever the gate fails.  Both reward files are
written with reward 0 before anything runs and rewritten at the end.  The
random seeds only drive the unscored correctness stages.

## Measurements on the scored seed (scale 3)

Pristine, and three candidates, measured with the harness in the agent image
(Ubuntu 24.04, g++ 13.3, CMake Release):

| workload | pristine WA | seek compactions off | old reference | sample candidate |
|---|---|---|---|---|
| `mixed` | 14.10 | 0.430 | 0.350 | 0.773 |
| `uniform` | 10.44 | 0.594 | 0.528 | 0.819 |
| `series` | 4.33 | 0.969, **reads 24x, time 6.2x** | 0.876, **reads 24x, time 6.2x** | 0.983 |
| `blob` | 5.00 | 0.863 | 0.735 | 0.850 |
| score | 1.0 | gate fails | gate fails | 0.853 |

* The untouched tree's ratio is 1.0 by construction (same source, same
  seed).  Two `harbor run -a nop` runs gave 1.0 and 1.00002.  `mixed`,
  `uniform` and `blob` are bit-identical run to run; `series` can differ by
  ~1e-4, because a long scan can trigger a read-sampled compaction that the
  worker thread runs while the scan is still going.  The declared tolerance
  is 0.01.
* Verifier runtime for the untouched tree: 233 s locally (declared 2700).
* `scripts/docker_failure_paths.sh`, 18 cases: nop 1 (1.0000), sample 1
  (0.8525); old-reference, seek-only (series reads 24x, time ~10x),
  tiered-constants (level-0 depth 12 everywhere), no-compaction, no-wal,
  format-change, big-memtable, wrong-get, scan-bug, crash-in-compaction,
  hang, planted-reward, verifier-exception, nobuild, missing, header-break
  all 0 with both reward files present.
* "Seek compactions off" is the one-line change that used to be the easy
  win.  On `series`, whose phase-F scans start inside the purged key range,
  the tombstones then stay put and phase-F reads grow 24x (limit 1.5x) and
  wall time 6.2x (limit 4x + 60 s).
* "Old reference" is the previous single-workload reference
  (`scripts/old_reference.patch`: seek compactions off, cheapest cold file
  first, grandparent-aligned outputs).  It fails the same way.
* "Sample candidate" (`scripts/sample_candidate.patch`) is the old
  reference's policy changes with seek compactions left on.  It passes
  every guardrail on every workload.
* Headroom: agents have taken `mixed` alone to 0.28 (see below), so a
  policy that gets most of that on `mixed` and `uniform` while keeping
  `series` scans cheap leaves plenty of room below 0.85.  No lower bound is
  known, and no reference is shipped.

## History: why the task looks like this

The first version scored `mixed` alone, with a shipped reference at 0.35.

* Frontier probe (3 attempts, xhigh): 0.429, 0.456 and a guardrail failure,
  each agent stopping by itself after 21-36 minutes (5-12M tokens, far
  below the 200M floor).  `bench.py` then compared against seed 1's
  pristine, which writes more than the scored seed's, so the agents saw
  ~0.35 locally and thought they had matched the reference.
* With a truthful self-check and the reference stated, the easiness screen
  (medium effort) reached 0.284 and 0.305 within 34 minutes: compaction
  tuning for one workload is a short problem with an easy path, and the
  reference was weak.

Hence: four workloads that pull the policy in different directions under
the same guardrails, the self-check comparing against the scored seed's
baseline (`scored_baseline.json`, which does not reveal the seed), and no
reference, so the objective is open-ended.

## Calibration of `mixed` alone (scale 3, seeds 1-3, older contract)

| engine | write amplification |
|---|---|
| pristine | 14.8 - 16.5 (seek-compaction volume varies with the seed) |
| no seek-triggered compactions (one line) | 6.06 - 6.09 |
| + min-overlap file selection | 5.65 - 5.72 |
| no seek + grandparent-aligned outputs | 5.67 |
| old reference (no seek, cheapest cold file, aligned outputs) | 4.97 - 5.01 |
| constants only: no seek, L0 trigger 12, L1 = 40 MB | 4.70, but level-0 depth 12 (rejected) |

The pristine number moves with the seed, which is why the scored seed is
fixed.  Each workload's pristine measurement is taken live and must fall in
a sanity band (`BASELINE_WA_BAND` in `verify.py`).  `harbor check` was not
run (no API credentials on the build machine).
