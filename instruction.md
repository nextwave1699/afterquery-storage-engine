# Add range deletes, merge operators and column families to LevelDB 1.23

`/app/leveldb` is LevelDB 1.23 (`99b3c03b`), git tag `pristine`; `/opt/pristine` is an untouched copy. Only `/app/leveldb` is collected. Add three features:

* **Range deletes**: `DB::DeleteRange(begin, end)` and `WriteBatch::DeleteRange`, deleting every key in `[begin, end)` as one tombstone. They must cost the same however many keys they cover, let reads skip what they hide without reading it, stay fast with 100,000 tombstones, and get the space back through background compaction.
* **Merge operands**: a `MergeOperator` in `Options`, with `DB::Merge` and `WriteBatch::Merge`. Reads fold operands into the key's value, compactions may fold them early, and a merge must not read the current value.
* **Column families**: independent key spaces in one database, each with its own options, memtable and levels, sharing the log, sequence numbers and snapshots. Create and drop them at run time, write to several atomically in one batch, reopen with descriptors. One family's writes must not pin the shared log for the others.

All three must be correct under snapshots, iterators both ways, reopen, crashes at any engine event and compaction.
`/app/spec.md` is the contract: declarations, visibility rules in terms of sequence numbers, efficiency limits. The verifier compiles its own harness copy in `/app/bench` against your `include/`, so signatures must match.
## How it is checked

1. **Build**: the tree's CMake (`-DLEVELDB_BUILD_TESTS=OFF`), and the harness compiles against it.
2. **API**: 28 fixed checks (batch iteration and append, missing merge operator, empty and reversed ranges, write order in a batch, family create/drop/re-create, reopen with descriptors, per-family options).

3. **Conformance**: 12000 seeded scenarios of 300 operations and 400 of 3000, plus 1000 stock-only ones. Each mixes puts, deletes, range deletes and merges over up to four families, alone and in batches spanning families, with reads, scans both ways, walks mixing Seek/Next/Prev, three live snapshots, family create/drop, reopens and compactions, on varied buffer, file and block sizes, bloom filters and `reuse_logs`. Every read is checked against a model.
4. **Recovery**: 400 crashes at random `Env` events. The database must come back holding exactly the acknowledged writes (the one in flight may or may not be there), with the right families, and stay usable. Plus eleven fixed crash points.
5. **Compatibility**: a database without the new records, with only the default family, stays readable both ways with the untouched engine, including after a crash; six pristine-written databases are read and extended.
6. **Efficiency**: four cases measured with the kernel's I/O counters and CPU time (`/app/spec.md` section 5). Expanding a range into point deletes, read-modify-write merges, reading through covered data, read cost growing with the tombstone count, and holding every log until quiet families flush all fail.
7. **No regression**: stock workloads at full size, write amplification at most 1.10x the untouched engine's, plus the guardrails in `/app/bench/README.md`.

The gate needs every check. The score `feature_score` is the mean over API, conformance, recovery and efficiency of the fraction of checks passed; the untouched tree scores 0.
## Self-check

    python3 /app/bench/bench.py            # everything, on other seeds
    python3 /app/bench/bench.py --suite    # API, scenarios, recovery
    python3 /app/bench/bench.py --perf     # the efficiency cases

A failing scenario is reproducible alone (see `bench.py`'s docstring). The verifier runs the same counts on its own seeds.
## Constraints

* Keep the public API source-compatible, the on-disk formats unchanged for databases without the new records, and all I/O through the options' `Env`.
* The verifier uses its own copy of the harness and its own seeds: changing `/app/bench` has no effect.
