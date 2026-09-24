# storage-engine

Add **range deletes** (range tombstones), **merge operators** and **column
families** to a pinned LevelDB (1.23, commit `99b3c03b`): `DB::DeleteRange`,
`WriteBatch::DeleteRange`, `MergeOperator`, `Options::merge_operator`,
`DB::Merge`, `WriteBatch::Merge`, `DB::CreateColumnFamily`/`DropColumnFamily`,
`DB::Open` with descriptors, and a family argument on every read and write.
They have to be correct under snapshots, iterators in both directions, reopen,
crashes at any engine event and compaction, and efficient in the ways that
make the features worth having: a range delete costs the same whatever it
covers, reads skip what it hides, reads stay fast under 100,000 tombstones,
the space comes back through background compaction, a merge never reads the
value, and one family's writes never pin the shared log for the others.
Databases that never saw the new records, with only the default family, stay
byte-compatible with the untouched engine in both directions, and the stock
paths must not regress.

The agent's brief is `instruction.md`; the contract is `environment/spec.md`
(`/app/spec.md` in the image).

Paradigm: **implementation**.  The gate (`gate_passed`, = `reward.txt`)
needs every stage; the untouched tree fails it because the conformance suite
does not even compile against it (no `leveldb/merge_operator.h`).  The
objective `feature_score` (higher, baseline 0.0) is the mean over the four
extension stages (api, conformance, recovery, efficiency) of the fraction of
their checks that passed; the reference scores 1.0.

Who would use a solution: anyone running LevelDB who needs to drop a key
range (a tenant, a time window, an index) without a read-modify-write of
every key, counters and append-only values without a read per write, or
several independent key spaces in one database with one write-ahead log and
consistent snapshots across them.  These are the three features most often
cited as reasons to move from LevelDB to RocksDB.

## Layout

| path | role |
|---|---|
| `instruction.md` | the brief |
| `environment/` | agent image: toolchain, `/app/leveldb` (pristine tree, git tag `pristine`), `/opt/pristine` (untouched copy), `/app/spec.md`, `/app/bench` (byte copies of the harness plus `bench.py`, the self-check) |
| `environment/sealed/leveldb.tar.gz` | the pinned source, built by `scripts/seal.sh` |
| `tests/` | verifier image, `test.sh`, `verify.py` (driver), `test_outputs.py` (verdict), `harness/` (`environment/bench` is a copy), `sealed/` (source + fixtures) |
| `solution/` | `reference-files.tar.gz` (the 34 changed and added files), `solve.sh` (what the oracle run executes: unpack, then check the new API is really there), and `reference.patch` for reading |
| `cheat/README.md` | shortcuts considered and why the verifier rejects them |
| `scripts/` | `seal.sh` (archives, fixtures, harness mirror), `gen_fixtures.sh`, `docker_failure_paths.sh` (trees through the verifier image), `mutants.py` (bugs the suite must catch) |
| `task.toml` | contract: gate `gate_passed`, objective `feature_score` |

## The harness

`tests/harness/conftest.cc` + `conf_model.{h,cc}` drive the engine through
the public API with the harness's own `leveldb::Env` (`bench_env.h`: POSIX
files, byte counters, a single background worker drained after every
operation so compactions happen at deterministic points, SIGKILL at a chosen
Env event).

* **Scenarios** (`conftest scenarios --ext`): a scenario is a pure function
  of its seed: engine shape (write buffer 32-96 KB per family, files
  48-144 KB, 512 B or 2 KB blocks, bloom filters, a 16 KB block cache,
  `reuse_logs`), key space (300-3000 keys), up to four column families that
  come and go during the run (creating and dropping one counts as a durable
  write, so recovery checks them too), and a stream of operations: single and batched puts,
  deletes, merges, range deletes (wide, narrow, empty, reversed, several in
  one batch around writes they must and must not hide, batches that span
  families), bursts of writes,
  point reads, forward and backward scans, iterator walks mixing Seek, Next
  and Prev, up to three live snapshots (checked with reads, scans and walks,
  also after compactions), reopens, and full or partial compactions.  The
  model is a `std::map`; every read is compared with it.  After the stream:
  a snapshot check of everything still held, a reopen and a full compaction,
  each followed by a full scan.
* **Merge operator**: `x -> a*x + b (mod 2^61-1)` on a 19-digit state,
  padded to the length of the value it lands on (so merged values stay as big
  as the values they replace).  Not commutative: order, duplication and loss
  all change the value.  `PartialMerge` composes two maps.
* **Recovery** (`conftest crash` / `recover`): a scenario with a crash armed
  at the N-th occurrence of an Env event (WAL append, table append/close/
  sync, MANIFEST append/sync, file removal, rename; before or after).  The
  recovered database must equal the model after the acknowledged writes (or
  one more: the batch in flight), then take writes, reopen and read back.
* **API** (`conftest api`): 28 fixed checks (batch Iterate/Append/copy with
  the new records, handlers that only override Put/Delete, Merge without an
  operator and a batch holding one, empty and reversed ranges, range bounds,
  write order within one batch, merge after delete, reopen and compaction;
  separate key spaces, batches across families, per-family merge operators,
  snapshots across families, ListColumnFamilies, opening without or with the
  wrong descriptors, create_missing_column_families, dropping the default
  family, a dropped handle, and re-creating a dropped name).
* **Efficiency** (`conftest perf`): four cases printing kernel I/O counters
  per phase, table bytes, live log bytes and CPU time; limits in `spec.md`
  section 5 and in `PERF_LIMITS` (`tests/verify.py`).

The stock stages reuse the earlier harness unchanged: `lsmbench.cc` +
`workload.h` (the `mixed` workload for the small cross-read, eleven fixed
crash points and the six sealed fixtures; `mixed`, `ttl`, `bimodal` at scale 3
for the regression benchmark, measured from `/proc/<pid>/io` of the stopped
harness process).

## Calibration (scale 1, this machine)

| case | metric | limit | reference | naive* |
|---|---|---|---|---|
| rangedel | A: bytes written by 400 range deletes | 512 KiB | 17.6 KB | 6.4 MB |
| rangedel | A: bytes read | 512 KiB | 0.1 KB | 32 MB |
| rangedel | S: bytes read by scans both ways + 2000 reads | 0.5 x load (17 MB) | 9.8 MB | 90 MB |
| rangedel | R: table bytes after reopen, no CompactRange | 0.3 x load + 1 MiB | 3.4 MB | 25.6 MB |
| rangedel | B: table bytes after CompactRange | 0.15 x load + 1 MiB | 3.4 MB | 3.4 MB |
| manyranges | CPU time, 100k range deletes + 200k reads | 15 s | 1.9 s | 1.7 s |
| merge | A: bytes written by 400k merges | 150 MiB | 22 MB | 714 MB |
| merge | A: bytes read | 256 MiB | 0.5 MB | 1.7 GB |
| merge | B: table bytes after CompactRange | 1.25 x load | 8.56 MB (1.0x) | 8.56 MB |
| cfwal | live .log bytes after 60 MB into one of four families | 12 MiB | 0.75 MB | - |
| cfwal | bytes written | 400 MiB | 207 MB | - |

\* naive = range delete expanded into point deletes, merge as
read-modify-write.  Other designs measured on the way: a global tombstone
index rebuilt when the memtable changes (the first version of the
reference) took 16.6 s on `manyranges` (over the limit); per-read linear
scans of the memtable's tombstones are O(n) per read (tens of seconds).
Not folding operands in compaction leaves ~20 MB of operands (2.3x load).

Reference, full verifier (applied by `solution/solve.sh`, as the oracle run
does): every stage passes, `feature_score` 1.0, stock write amplification
0.99x the pristine's (limit 1.10x), 7 minutes.  Through the verifier image:

| tree | gate | feature_score |
|---|---|---|
| reference | 1 | 1.0000 |
| untouched | 0 | 0.0000 |
| the API present but range deletes doing nothing | 0 | 0.4620 |
| naive (point deletes, read-modify-write merges) | 0 | 0.9737 (only efficiency) |
| a build that writes the reward files | 0 | 0.0000 |

Conformance: 15,600 short and 300 long extra scenarios on other seeds, plus
4,800 short and 180 long with column families, all pass on the reference;
200 extra randomized crash-recovery cases pass.

## Mutation check

`scripts/mutants.py` lists small bugs injected into the reference; the
suite must fail each.  Results (300 scenarios each, or a hang, which the
verifier's per-chunk timeout turns into a failure):

| mutant | scenarios passed |
|---|---|
| iterator ignores range tombstones | 0/300 |
| Get ignores range tombstones | 0/300 |
| compaction drops covered entries across snapshots | 2/300 |
| compaction drops tombstones | 0/300 |
| iterator folds operands in reverse order | 4/300 |
| PartialMerge arguments swapped in compaction | 75/300 |
| memtable flush loses tombstones | 0/300 |
| compaction folds operands across snapshots | 0/300 |
| memtable Get stops at the first operand | 40/300 |
| layer-skipping iterator ignores layers | 0/300 |
| Get skip ignores layers (versions / memtables) | 7/300, 7/300 |
| backward skip lands inside the range | hangs |
| fragment split loses sequence numbers | 52/300 |
| a family's drop is not recorded | 18/300 |
| the shared log is never trimmed for lagging families | caught by `cfwal` (59.5 MB of live logs against 12 MiB) |
| a flush claims the current log instead of its memtable's | caught by the verifier's randomized crashes |
| recovery applies each flush to the manifest as it goes | caught by the verifier's randomized crashes (this was a real bug, found at 390/400) |

(One more, "reverse scan keeps operands after a deletion", is equivalent to
the reference: the merge branch resets the operand list anyway.)

Through the full verifier image (`scripts/docker_failure_paths.sh`), every
mutant gets gate 0 with feature_score 0.45-0.72; the untouched tree 0.0; a
stub that compiles 0.40; the naive engine 0.90 (efficiency only); breaking
stock guarantees (format change, new MANIFEST tag on every table, no WAL,
lost recovery batches, 64 MB memtables, no compaction, a compaction that
aborts or hangs, planted reward files) all gate 0; the reference gate 1 and
1.0.  A hanging candidate is bounded by per-stage deadlines (conformance and
recovery 20 min each) and finishes in about an hour.

## Packaging note

The reference ships as a tarball, not as a patch applied by `solve.sh`.  An
earlier bundle shipped only `reference.patch`, and the pipeline's oracle run
came out identical to the untouched run (gate 0, `feature_score` 0.0000,
same runtime): the patch had not applied there, most likely because the
upload rewrote its line endings.  A tarball survives that, `solve.sh` now
fails loudly when the tree does not end up with the new API, and
`scripts/docker_failure_paths.sh` runs `solve.sh` itself so the oracle path
is what gets tested.

## Rebuilding

    scripts/seal.sh              # archives, fixtures, mirror tests/harness -> environment/bench
    docker build -t lsm-verifier tests
    scripts/docker_failure_paths.sh   # nop, reference, naive, mutants through the verifier image
