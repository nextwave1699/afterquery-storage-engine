# Shortcuts and why the verifier rejects them

Each entry was reasoned through against `tests/verify.py`; the runnable ones
are exercised by `scripts/docker_failure_paths.sh` (case name in brackets).
None is executed by the pipeline.

| shortcut | what happens |
|---|---|
| **Expand a range delete into point deletes** [naive]. | Correct, and passes every scenario, but the `rangedel` case measures it: phase A writes 6.4 MB and reads 32 MB (limits 512 KiB each), and the tombstones in the scans' way still get read (S: 90 MB against 17 MB). |
| **Merge by read-modify-write** [naive]. | Correct, but 400k merges write 714 MB and read 1.7 GB (limits 150 and 256 MiB). |
| **Keep operands forever** (never fold in compaction). | Reads stay correct; after `CompactRange` the tables hold ~2.3x the load (limit 1.25x). |
| **Hide covered keys one by one in the iterator** [no-skip]. | Correct, but full scans read every covered block: phase S reads ~2.6x the load (limit 0.5x). |
| **One global tombstone list rebuilt or scanned per read.** | `manyranges` (100k tombstones, 200k reads) takes tens of seconds to minutes of CPU (limit 15 s; the reference 1.9 s). |
| **Leave the space to `CompactRange`** [no-rd-compaction]. | After a reopen with no compaction asked for, the tables still hold ~75% of the load (limit 0.3x). |
| **Stubs that compile** [stub]. | The suite compiles, so the score is partial, but almost every scenario with a range delete fails; the gate fails. |
| **Tag every table with the new MANIFEST record** [always-new-tag]. | Stock databases must stay readable by the untouched engine: the cross reads, crash directories and fixtures fail with "unknown tag". |
| **Change the table format** [format-change]. | Same: the pristine engine reads every stock database the candidate writes, with checksums verified. |
| **Report fake statistics or patch the harness.** | Nothing the engine prints is used: every read is checked against the harness's model, I/O comes from `/proc/self/io` of the harness process (and `/proc/<pid>/io` of the stopped process for the benchmark), and the verifier compiles its own copy of the harness from `/tests/harness`. |
| **Write files outside the `Env`** (direct `open`/`write`, `mmap`). | The kernel counters count every write of every thread whatever the path; bytes can only be counted more, never less. |
| **Defer work past the measured phase** (background thread, `atexit`, a helper process). | Phase A of `rangedel` is followed by scans and a reopen that are measured too; deferred expansion shows up in S or R.  Work after `main()` never runs for the benchmark (the process is stopped and killed), and every process of the unprivileged user is killed after each step. |
| **Lose or reorder writes on recovery** [recdrop, no-wal]. | 400 randomized crash cases and eleven fixed ones check that exactly the acknowledged writes come back (the in-flight batch wholly or not at all). |
| **Drop entries a snapshot still needs** [mutants]. | Scenarios hold up to three snapshots across compactions and read through each; see the mutation table in README.md. |
| **Detect the harness and behave differently.** | Scenario seeds, crash points and the benchmark seed are sealed in the verifier and differ from `bench.py`'s; the efficiency cases are fixed but read-checked against the model throughout. |
| **Write the reward files or the report from the build** [planted-reward]. | The build runs as the unprivileged user; `/logs/verifier` and the report are root-owned, `/tests` is unreadable; the reward is written last by root after clearing the paths. |
| **Modify `/app/bench`.** | The verifier compiles its own copies of the harness from `/tests/harness`. |
