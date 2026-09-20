# storage-engine

Change the compaction strategy of a pinned LevelDB (1.23, commit `99b3c03b`)
so that twelve fixed LSM-tree workloads are written with as few bytes as
possible, while every read stays correct, crash recovery works at every
engine event for both the candidate and the pristine engine, the on-disk
format stays byte-compatible in both directions, and level-0 depth, space,
read bytes and memory stay inside the baseline's envelope on every workload.
The agent's brief is `instruction.md`; `environment/bench/README.md` (in the
image as `/app/bench/README.md`) describes the workloads.

Paradigm: performance, no reference solution (so no solve bar; the score is
open-ended).  The gate (`gate_passed`, = `reward.txt`) is every correctness
stage plus every workload's guardrails; the untouched tree passes it.  The
objective is `write_amp_ratio` = geometric mean over twelve workloads of
candidate write amplification / pristine write amplification, both measured
in the same verifier run on a fixed sealed seed (`BENCH_SEED` in
`tests/verify.py`).  Lower is better; the untouched tree scores 1.0.

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
fixtures and the crash/compat/differential stages use only it).  The other
eleven are recipes in one table at the top of `workload.h`: a key
distribution (uniform, scattered hot set, rolling cursor, append with late
corrections, bimodal, sparse) plus rates for deletes, reads, scans, batch
size, burstiness and value lengths.  `environment/bench/README.md` has the
table with sizes and pristine write amplification.

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
6. all twelve workloads at scale 3 and `BENCH_SEED` with both engines, cross
   reads at full scale, guardrails per workload, and the score.  493 s for
   the untouched tree locally; `verifier_runtime_sec` is declared at 2700.

`reward.txt` and `gate_passed` are 1 when every stage and every guardrail
hold.  `write_amp_ratio` and the informational `write_amp_ratio_<workload>`
keys are reported as 1.0 whenever the gate fails.  Both reward files are
written with reward 0 before anything runs and rewritten at the end.  The
random seeds only drive the unscored correctness stages.

## Measurements on the scored seed (scale 3)

Pristine and two sample engines, measured with the harness in the agent
image (Ubuntu 24.04, g++ 13.3, CMake Release).  "Seek off" is
`scripts/old_reference.patch` without its other changes: the one-line
strategy that used to win.  "Sample" is `scripts/sample_candidate.patch`.

| workload | pristine WA | seek off | sample |
|---|---|---|---|
| `mixed` | 14.10 | 0.430 | 0.773 |
| `uniform` | 10.44 | 0.594 | 0.819 |
| `hotkey` | 3.54 | 0.893 | 0.916 |
| `rolling` | 7.23 | 0.688 | 0.712 |
| `bursts` | 15.43 | 0.392 | 0.538 |
| `smallval` | 9.72 | 0.576 | 0.803 |
| `blob` | 5.13 | 0.861 | 0.845 |
| `wide` | 9.55 | 0.772 | 0.855 |
| `scan` | 8.00 | 0.626 | 0.869 |
| `bimodal` | 5.08 | 0.833 | 0.821 |
| `series` | 4.33 | **fails** (reads 24x) | 0.983 |
| `ttl` | 3.74 | **fails** (reads 26x) | 1.042 |
| **score** | **1.0** | **0.690, but gate fails** | **0.8213** |

* The untouched tree scores 1.0 by construction and the full verifier
  measured exactly 1.0000 (493 s locally).
* Seek-triggered compactions off is the strategy every earlier agent found.
  It is worth 0.43 on `mixed` and 0.39 on `bursts`, but on `series` and
  `ttl` the ranges purged in key order are never compacted away, so phase-F
  scans read 24x and 26x the baseline and the gate fails.  Even if it
  passed, spread over twelve workloads it averages 0.690, because the
  workloads with little headroom (`hotkey` 3.5 pristine WA, `ttl` 3.7,
  `series` 4.3, `blob` 5.1) pull it back towards 1.0.
* The sample engine keeps seek compactions and adds min-overlap-per-byte
  input selection with a hotness weight plus grandparent-aligned outputs.
  It passes every guardrail on all twelve and scores 0.8213.
* `ttl`'s phase-F read guardrail is 2.5x rather than 1.5x: only a tenth of
  its ids stay live, so scans cross long tombstone runs and read cost
  swings much harder there.  The sample engine measures 1.7x; the
  pile-up case is 26x, so the limit still catches it.
* Headroom: agents took `mixed` alone to 0.28 (see below), and no workload
  here is near that, so there is a lot of room below the sample's 0.82.  No
  lower bound is known and no reference is shipped.

### The flush side: measured, not assumed

`bimodal` was built on the idea that a memtable holding both tail appends
and low-key updates flushes to one level-0 file overlapping everything
beneath it, so splitting flush output should pay.  Two prototypes on top of
the sample engine (`WriteLevel0Table` in `db/db_impl.cc` writing several
tables per memtable) say otherwise:

| variant | `bimodal` | `mixed` | `rolling` | `series` | `ttl` |
|---|---|---|---|---|---|
| sample | 0.821 | 0.773 | 0.712 | 0.983 | 1.042 |
| split by size (2 MB) | 1.145 | 0.810 | 0.840 | 1.328 | 1.136 |
| split at level-1 boundaries | 1.143 | 0.815 | 0.875 | 1.172 | 1.226 |

Both are worse everywhere: at a 4 MB write buffer the extra level-0 files
cost more than the reduced overlap saves.  So the task does not *force* a
candidate through the flush path, and neither the instruction nor this
README claims it does.  What the corpus does is make single-lever policies
plateau.  The prototypes are not in the bundle; they are recorded here so a
reviewer knows the avenue was measured rather than assumed.

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

Hence: workloads that pull the policy in different directions under the
same guardrails, the self-check comparing against the scored seed's
baseline (`scored_baseline.json`, which does not reveal the seed), and no
reference, so the objective is open-ended.

With four workloads (2026-09-18): the easiness screen passed (medium, one
attempt, 0.555 in 34 minutes, no solve bar).  The frontier probe's three
xhigh attempts all passed the gate with different policies, at 0.565, 0.610
and 0.616 (mixed 0.38-0.41, uniform 0.54-0.59, series 0.65-0.90, blob
0.73-0.81), and their `bench.py` estimates matched the verifier within
about 1%.  Each agent reported completion after about 48 minutes, with
11-18M tokens, so the token floor failed again: the search space was a
dozen labelled compaction knobs in one file and the measure loop was
seconds long, so a competent agent converged in under an hour.

With twelve workloads (2026-09-20): the corpus now spans skewed overwrites,
sequential sweeps, bursts, tiny and large values, sparse writes,
scan-heavy reads, interleaved appends/updates and TTL purges, all under the
same guardrails.  A policy that only picks compaction inputs better cannot
carry all of them: the strategy those three agents converged on averages
about 0.7 here and the seek-off lever fails the gate outright.  Whether
anything much below the sample's 0.82 is reachable without new machinery is
open; the flush-side prototypes above did not get there.  The instruction
also says outright that there is no finish line and that results are
ranked.

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
