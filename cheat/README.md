# Shortcuts and why the verifier rejects them

Each entry was reasoned through against `tests/verify.py`; the runnable ones
are exercised by `scripts/docker_failure_paths.sh` (case name in brackets).
None is executed by the pipeline.

| shortcut | what happens |
|---|---|
| **Report fake statistics** (`leveldb.stats`, `GetApproximateSizes`, a patched harness). | Nothing the engine prints or the harness computes in-process is the score. Bytes written come from `/proc/<pid>/io` of the harness process, read by a helper while the process is stopped; logical bytes from the pristine harness's `describe`; the level layout from a Python replay of the MANIFEST. |
| **Write files outside the `Env`** (direct `open`/`write`, `mmap`). | The kernel counter covers every write of every thread regardless of the path; bytes are only ever counted more, never less. |
| **Defer work past the end** (`atexit`, static destructors, a helper process). | The harness stops itself with SIGSTOP after closing the database; the helper reads the counters and then SIGKILLs it, so nothing after `main()` runs. All processes of the unprivileged user are killed after every step. |
| **Disable compaction** [no-compaction]. | Write amplification drops to ~2 but level 0 grows to hundreds of files: the level-0 depth guardrail (<= 4 overlapping files at any version, from the MANIFEST history) fails, as do peak space and phase-F read bytes. |
| **Raise the level-0 trigger / enlarge level 1** [tiered-constants]. | Reaches 4.7 with 12 level-0 files: level-0 depth 12 > 4. Level-1 sizing alone (30-40 MB) is no longer a win at the verifier's scale (6.07) and the peak-space guardrail (1.25x) bounds it anyway. |
| **Skip the write-ahead log** [no-wal]. | Saves 1.0x of write amplification; the crash scenarios recover nothing since the last flush, and the `log_append` trigger never fires (fail). |
| **Bigger memtables than `write_buffer_size`** [big-memtable]. | Fewer memtable switches than 0.8x the baseline (counted from `log_number` changes in the MANIFEST), tables over 8 MB, and the peak-RSS bound. |
| **Compress or re-encode tables / WAL / MANIFEST** [format-change]. | The pristine engine reads every candidate database (small workload, all crash directories, full-scale benchmark output, extended fixtures) with checksums verified; an unknown compression type, magic number or record tag fails there. |
| **Return cached or precomputed results** [wrong-get, scan-bug]. | Every read is compared with the model; keys, values, order, deletions and snapshots are all checked, across seeds drawn at verify time. |
| **Detect the benchmark and behave differently.** | The seed changes per run, the workload's structure is what the agent was given, and the crash/compat/differential workloads use further seeds; there is no unmeasured path to take. |
| **Reorder level-0 files or put overlapping files in level >= 1.** | The pristine engine's `Get` consults level-0 files newest-file-number-first and binary-searches levels >= 1; the cross reads at full scale (10k point reads, forward and reverse full scans) fail. |
| **Break durability semantics subtly** (return before the WAL append, lose the in-flight batch's neighbours). | Every crash scenario checks that all acknowledged batches are present and that the in-flight batch is either wholly present or wholly absent, by both engines. |
| **Write the reward files or the report from the build** [planted-reward]. | The build runs as the unprivileged user; `/logs/verifier` and the report are root-owned, `/tests` is unreadable; the reward is written last by root after clearing the paths. |
| **Modify `/app/bench`.** | The verifier compiles its own copies of the harness from `/tests/harness`. |
| **Do only the obvious thing** [seek-only]. | Disabling seek-triggered compactions alone measures 6.06-6.09; the bar is 5.40. Adding min-overlap selection alone reaches 5.65-5.72. Several policy changes are needed. |
