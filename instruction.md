# Add range deletes and merge operators to LevelDB 1.23

`/app/leveldb` is LevelDB 1.23 (`99b3c03b`) with a `pristine` git tag; `/opt/pristine` is an untouched copy. Only `/app/leveldb` is collected.

Add two features the engine does not have:

* **Range deletes**: `DB::DeleteRange(begin, end)` and `WriteBatch::DeleteRange`, deleting every key in `[begin, end)` as one tombstone. They must work with snapshots, iterators in both directions, reopen, crash recovery and compaction. They must cost about the same however many keys they cover, let scans and point reads skip what they hide without reading it, keep reads fast under 100,000 tombstones, and get their space back through normal background compaction.
* **Merge operands**: a `MergeOperator` in `Options`, with `DB::Merge` and `WriteBatch::Merge`. Reads fold the operands into the key's value, and compactions may fold them early. A merge must not read the current value.

`/app/spec.md` is the contract: the exact declarations, the visibility rules in terms of sequence numbers, and the efficiency limits. The verifier compiles its own copy of the harness in `/app/bench` against your `include/`, so the signatures must match.

## How it is checked

1. **Build**: the tree's CMake (`-DLEVELDB_BUILD_TESTS=OFF`) builds it, and the harness compiles against it.
2. **API**: fixed checks of the new calls (batch iteration and append, missing merge operator, empty and reversed ranges, write order inside one batch).
3. **Conformance**: 12000 seeded scenarios of 300 operations and 400 of 3000 operations. Each mixes puts, deletes, range deletes and merges, alone and in batches, with point reads, full scans both ways, iterator walks mixing Seek/Next/Prev, up to three live snapshots, reopens and compactions, on varied buffer, file and block sizes, bloom filters, a small block cache and `reuse_logs`. Every read is compared with a model. Plus 1000 stock-only scenarios.
4. **Recovery**: 400 crashes at random `Env` events (WAL, table, MANIFEST, file removal, rename). The database must come back holding exactly the acknowledged writes (the one in flight may or may not be there), and must stay usable. Plus eleven fixed crash points in a larger stock workload.
5. **Compatibility**: a database that never saw the new records stays readable both ways with the untouched engine, including after a crash, and six pristine-written databases are read and extended.
6. **Efficiency**: three cases measured with the kernel's I/O counters and CPU time (`/app/spec.md` section 4). Expanding a range into point deletes, merging by read-modify-write, reading through covered data, or a read cost that grows with the number of tombstones all fail them.
7. **No regression**: stock workloads at full size, write amplification at most 1.10x the untouched engine's, plus the guardrails in `/app/bench/README.md`.

The gate needs every check to pass. The score, `feature_score`, is the mean over API, conformance, recovery and efficiency of the fraction of their checks that passed; the untouched tree scores 0.

## Self-check

    python3 /app/bench/bench.py            # everything, on seeds the verifier does not use
    python3 /app/bench/bench.py --suite    # API, scenarios, recovery (~2 min)
    python3 /app/bench/bench.py --perf     # the efficiency cases
    python3 /app/bench/bench.py --bench    # the stock regression benchmark (~6 min)

A failing scenario is reproducible alone (see the docstring of `bench.py`). The verifier runs the same counts on its own seeds, so rare failures matter: a bug that shows up once in 20,000 scenarios can still fail the gate.

## Constraints

* Keep the public API source-compatible. Keep the on-disk formats unchanged for databases without the new records, and route all I/O through the `Env` in the options.
* The verifier uses its own copy of the harness and its own seeds, so changing `/app/bench` has no effect.
