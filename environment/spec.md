# Range deletes and merge operators for LevelDB 1.23: the contract

The verifier compiles its own harness (the sources are in `/app/bench`)
against your `include/leveldb/` and links it with the `libleveldb.a` your
tree's CMake builds.  The declarations below must exist with these
signatures, and the semantics below are what every read is checked against.
Anything not mentioned keeps working exactly as it does today.

Sequence numbers are LevelDB's existing per-write sequence numbers: every
record of a `WriteBatch` consumes one, in batch order, including the new
records below (an empty range still consumes one).

## 1. API

    // include/leveldb/merge_operator.h   (new header)
    namespace leveldb {
    class LEVELDB_EXPORT MergeOperator {
     public:
      virtual ~MergeOperator();
      virtual const char* Name() const = 0;
      // Fold `operands` (oldest first) into `existing` (nullptr when the key
      // has no value underneath them) and store the result in *new_value.
      virtual bool FullMerge(const Slice& key, const Slice* existing,
                             const std::vector<std::string>& operands,
                             std::string* new_value) const = 0;
      // Optionally fold two adjacent operands, `left` older than `right`, into
      // one with the same effect.  The default returns false ("cannot").
      virtual bool PartialMerge(const Slice& key, const Slice& left,
                                const Slice& right, std::string* result) const;
    };
    }

    // include/leveldb/options.h, in struct Options
    const MergeOperator* merge_operator = nullptr;

    // include/leveldb/db.h, in class DB (virtual, not pure: DB subclasses
    // that do not override them must still compile)
    virtual Status DeleteRange(const WriteOptions& options,
                               const Slice& begin, const Slice& end);
    virtual Status Merge(const WriteOptions& options, const Slice& key,
                         const Slice& value);

    // include/leveldb/write_batch.h
    void WriteBatch::DeleteRange(const Slice& begin, const Slice& end);
    void WriteBatch::Merge(const Slice& key, const Slice& value);
    // in WriteBatch::Handler; the defaults do nothing, so existing handlers
    // that only override Put and Delete still compile and still work
    virtual void Handler::DeleteRange(const Slice& begin, const Slice& end);
    virtual void Handler::Merge(const Slice& key, const Slice& value);

`WriteBatch::Iterate` reports every record in batch order, the new ones
through the new handler methods; `Append`, copies and `ApproximateSize`
include them.

## 2. Range deletes

`DeleteRange(begin, end)` deletes every key `k` with `begin <= k < end`
under the options' comparator.  `begin >= end` deletes nothing.  In a batch
it applies atomically with the rest of the batch, in batch order, and it is
exactly as durable as `Put` and `Delete`: once `Write` returns, a crash does
not lose it, and a crash before that loses the whole batch or none of it.

Visibility, relative to sequence numbers: an entry of key `k` written at
sequence `s` is deleted for a reader at sequence (snapshot) `r` if some range
delete covering `k` has a sequence number `t` with `s < t <= r`.  So:

* a write after the range delete (a later batch, or later in the same batch)
  is visible; one before it is not;
* a snapshot taken before the range delete, and an iterator created before
  it, still see the old entries;
* `Get`, iterators in both directions (`Seek`, `SeekToFirst`, `SeekToLast`,
  `Next`, `Prev` in any mix) and reads through snapshots all honour it.

After `CompactRange(nullptr, nullptr)` with no live snapshot, the data a
range delete covers is gone from the table files, not merely hidden.

## 3. Merge operands

`Merge(key, operand)` records an operand.  A read of `key` at sequence `r`
returns `FullMerge(key, base, operands)`, where `operands` are the operands
of `key` visible at `r` that are newer than its newest visible `Put`,
`Delete` or covering range delete (oldest first), and `base` is that `Put`'s
value, or nullptr if it was a deletion or there is none.  A key whose newest
visible entry is an operand exists (`Get` returns OK, iterators return it).

* `Get`, iterators in both directions and snapshot reads all return merged
  values; a snapshot sees exactly the operands written before it.
* `Merge` without `options.merge_operator`, or a `Write` of a batch holding
  an operand without one, returns `Status::InvalidArgument` and applies
  nothing.  `DeleteRange` does not need a merge operator.
* Compactions may fold operands with `PartialMerge`, or with `FullMerge`
  once the base (or the certainty that there is none) is in hand, as long as
  no reader, at any live snapshot, can tell.  After
  `CompactRange(nullptr, nullptr)` with no live snapshot, every key holds a
  single plain value again.

The harness's operator is not commutative (`x -> a*x + b` modulo a prime),
and `PartialMerge` always succeeds for it.  Operands applied out of order,
twice, or not at all give a different value.

## 4. Column families

A column family is an independent key space inside one database, with its own
options, its own memtable and its own levels, sharing the database's write-ahead
log, sequence numbers and snapshots.

    // include/leveldb/db.h
    class LEVELDB_EXPORT ColumnFamilyHandle {
     public:
      virtual ~ColumnFamilyHandle();
      virtual const std::string& GetName() const = 0;
      virtual uint32_t GetID() const = 0;
    };

    struct LEVELDB_EXPORT ColumnFamilyDescriptor {
      std::string name;
      Options options;
    };

    // in class DB
    static Status Open(const Options& db_options, const std::string& name,
                       const std::vector<ColumnFamilyDescriptor>& families,
                       std::vector<ColumnFamilyHandle*>* handles, DB** dbptr);
    static Status ListColumnFamilies(const Options& options,
                                     const std::string& name,
                                     std::vector<std::string>* names);
    virtual Status CreateColumnFamily(const Options& options,
                                      const std::string& name,
                                      ColumnFamilyHandle** handle);
    virtual Status DropColumnFamily(ColumnFamilyHandle* handle);
    virtual Status DestroyColumnFamilyHandle(ColumnFamilyHandle* handle);
    virtual ColumnFamilyHandle* DefaultColumnFamily() const;

    // every read and write takes a family (the existing ones use the default)
    virtual Status Put(const WriteOptions&, ColumnFamilyHandle*,
                       const Slice& key, const Slice& value);
    virtual Status Delete(const WriteOptions&, ColumnFamilyHandle*, const Slice& key);
    virtual Status Merge(const WriteOptions&, ColumnFamilyHandle*,
                         const Slice& key, const Slice& value);
    virtual Status DeleteRange(const WriteOptions&, ColumnFamilyHandle*,
                               const Slice& begin, const Slice& end);
    virtual Status Get(const ReadOptions&, ColumnFamilyHandle*,
                       const Slice& key, std::string* value);
    virtual Iterator* NewIterator(const ReadOptions&, ColumnFamilyHandle*);
    virtual void CompactRange(ColumnFamilyHandle*, const Slice* begin,
                              const Slice* end);
    virtual bool GetProperty(ColumnFamilyHandle*, const Slice& property,
                             std::string* value);

    // include/leveldb/write_batch.h, in WriteBatch
    void Put(ColumnFamilyHandle*, const Slice& key, const Slice& value);
    void Delete(ColumnFamilyHandle*, const Slice& key);
    void Merge(ColumnFamilyHandle*, const Slice& key, const Slice& value);
    void DeleteRange(ColumnFamilyHandle*, const Slice& begin, const Slice& end);
    // in WriteBatch::Handler; the defaults call the existing methods when the
    // family is the default one (id 0) and ignore the record otherwise, so
    // handlers written against the old interface still compile and still work
    virtual void PutCF(uint32_t cf, const Slice& key, const Slice& value);
    virtual void DeleteCF(uint32_t cf, const Slice& key);
    virtual void MergeCF(uint32_t cf, const Slice& key, const Slice& value);
    virtual void DeleteRangeCF(uint32_t cf, const Slice& begin, const Slice& end);

    // include/leveldb/options.h, in struct Options
    bool create_missing_column_families = false;

Rules:

* The family named `default` always exists, has id 0, and is what every call
  without a family uses.  `DB::Open` without descriptors opens it alone, and
  fails with `InvalidArgument` if the database has other families.
* `Open` with descriptors must name every family in the database, in any
  order, and fills `*handles` in the order given.  A named family that does
  not exist is `InvalidArgument` unless `create_missing_column_families` is
  set, in which case it is created.  Each descriptor's `Options` apply to
  that family: `write_buffer_size`, `max_file_size`, `block_size`,
  `filter_policy`, `merge_operator`, `compression`.  The database-wide
  options come from the `Options` passed to `Open`.
* Ids are assigned when a family is created and are never reused, not even
  after a drop and a re-create of the same name.  `ListColumnFamilies` reads
  the names from a closed database.
* A family is a separate key space: the same key in two families is two
  entries with two values, and an iterator over one never returns another's.
  Range deletes, merge operands and snapshots all apply per family.
* One `WriteBatch` may touch several families and applies atomically to all
  of them; records take sequence numbers in batch order, from the one
  sequence space the database has.  A snapshot is a point in that space and
  reads consistently from every family.
* After `DropColumnFamily`, reads and writes through that handle return
  `InvalidArgument`, the name may be created again as a new family, and the
  dropped family's tables are deleted from disk.  The handle must still be
  released with `DestroyColumnFamilyHandle`.  Dropping the default family is
  `InvalidArgument`.
* Crash safety covers all of it: acknowledged writes to every family survive,
  as do creations and drops, and a crash never resurrects a dropped family
  or loses another family's data.
* A database whose only family is `default` stays byte-compatible with the
  untouched engine, in both directions, exactly as in section 5.  Once
  another family exists, only your engine has to read the database.

## 5. Efficiency

A range delete must cost about the same whatever it covers; reads must not
wade through what a range delete hides once a newer layer's tombstone
proves it hidden; the cost of a read must not grow with the number of
tombstones; the space must come back without anyone asking; and a merge
must not read the key's value.  `conftest perf` measures this with the
kernel's I/O counters of the process (`wchar`/`rchar` in `/proc/self/io`)
and its CPU time.  Stock sizes: 4 MB write buffer, 2 MB files, 4 KB blocks,
no compression, 10-bit bloom filters, a 1 MB block cache.  The table-byte
limits are relative to the table bytes after the load (`LOAD_TABLE_BYTES`,
about 34 MB for `rangedel`, 8.5 MB for `merge`).

`rangedel`: load 300,000 keys and compact everything.
**A**: 400 `DeleteRange` calls that together cover 90% of the keys.
**S**: a full forward scan, a full backward scan and 2,000 point reads,
while those tombstones are still in the memtable.
**R**: close, reopen, and wait for background work (no `CompactRange`).
**B**: `CompactRange(nullptr, nullptr)`.

| limit | value |
|---|---|
| bytes written in A (`A_WCHAR`) | <= 512 KiB |
| bytes read in A (`A_RCHAR`) | <= 512 KiB |
| bytes read in S (`S_RCHAR`) | <= 0.5 x `LOAD_TABLE_BYTES` |
| table bytes after R (`R_TABLE_BYTES`) | <= 0.3 x `LOAD_TABLE_BYTES` + 1 MiB |
| table bytes after B (`B_TABLE_BYTES`) | <= 0.15 x `LOAD_TABLE_BYTES` + 1 MiB |
| peak RSS (`VMHWM_KB`) | <= 1 GiB |

`manyranges`: load 100,000 keys and compact everything; then 100,000
`DeleteRange` calls over 1 to 3 keys each (half of them over keys that do
not exist), a `Put` after every fourth, two point reads after each, and a
20-step iterator walk after every thousandth.

| limit | value |
|---|---|
| CPU time of that loop, all threads (`M_CPU_MS`) | <= 15,000 ms |
| peak RSS (`VMHWM_KB`) | <= 1 GiB |

`cfwal`: open four families (`default`, `a`, `b`, `c`, 1 MB write buffers),
write 2 MB into each of `b` and `c` and then nothing more, then 60 MB into
`a` in small batches, then read everything back.  One family's writes must
not pin the shared log forever: the engine has to flush the families that
lag behind so old log files can go.

| limit | value |
|---|---|
| bytes of live `.log` files at the end (`W_LOG_BYTES`) | <= 12 MiB |
| bytes written during the phase (`W_WCHAR`) | <= 400 MiB |
| peak RSS (`VMHWM_KB`) | <= 1 GiB |

`merge`: load 20,000 keys with 400-byte values and compact everything.
**A**: 400,000 `Merge` calls on random keys; then reads.
**B**: `CompactRange(nullptr, nullptr)`, reopen, reads.

| limit | value |
|---|---|
| bytes written in A (`A_WCHAR`) | <= 150 MiB |
| bytes read in A (`A_RCHAR`) | <= 256 MiB |
| table bytes after B (`B_TABLE_BYTES`) | <= 1.25 x `LOAD_TABLE_BYTES` |
| peak RSS (`VMHWM_KB`) | <= 1 GiB |

Every read in these cases is checked against the model, like everywhere
else.  The exact code is `RunPerfCase` in `/app/bench/conf_model.cc`.

## 6. What stays the same

* Every existing behaviour of the public API and every public header stays
  source-compatible.
* A database that never saw a range delete or a merge operand stays
  byte-compatible both ways: the untouched LevelDB 1.23 reads (and extends)
  what your engine wrote, including after a crash, and your engine reads,
  extends and recovers databases the untouched engine wrote.  Once range
  deletes or operands have been written, the untouched engine does not need
  to read the database.
* All file I/O goes through the `Env` in the options (the harness supplies
  its own, to count bytes and inject crashes).
* No regression on stock workloads (`mixed`, `ttl`, `bimodal` from
  `/app/bench/workload.h`, scale 3): write amplification at most 1.10x the
  untouched engine's, plus the guardrails in `/app/bench/README.md`.
