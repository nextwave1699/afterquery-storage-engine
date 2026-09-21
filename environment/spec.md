# Range deletes and merge operands: the API the verifier expects

This is the contract.  The verifier compiles its own harness against your
`include/leveldb/`, so these declarations must exist with these signatures
and these semantics.  Anything not mentioned here keeps working exactly as
it does today.

Sequence numbers below are LevelDB's existing per-write sequence numbers:
every entry in a write batch gets one, in batch order.

## 1. Range deletes

    // include/leveldb/db.h
    virtual Status DeleteRange(const WriteOptions& options,
                               const Slice& begin, const Slice& end);

    // include/leveldb/write_batch.h
    void DeleteRange(const Slice& begin, const Slice& end);

`DeleteRange(begin, end)` deletes every key in `[begin, end)`: begin
included, end excluded.  `begin >= end` is a no-op.  In a `WriteBatch` it
applies atomically with the rest of the batch, in batch order, and it is
durable exactly like `Put` and `Delete`: once `Write` returns, a crash must
not lose it.

Visibility rules, all relative to sequence numbers:

* A read at sequence `s` sees a key `k` as deleted if some range tombstone
  covering `k` has a sequence number `> the key's write` and `<= s`.
* A `Put` after a range tombstone is visible again; the tombstone does not
  affect later writes.
* A snapshot taken before the tombstone still sees the old values, and an
  iterator created before it keeps its own view.
* `Get`, `NewIterator` (forward and backward), and `GetApproximateSizes`
  must all honour tombstones.  Keys hidden by a tombstone are not returned
  by iteration and `Get` returns `NotFound`.

Range tombstones must be dropped once nothing can see them: after a
`CompactRange(nullptr, nullptr)` with no live snapshot, the data covered by
them is gone from disk (the verifier checks the on-disk bytes shrink).

## 2. Merge operands

    // include/leveldb/merge_operator.h   (new header)
    namespace leveldb {
    class MergeOperator {
     public:
      virtual ~MergeOperator();
      // Name is recorded in the database; opening with a different name is
      // an error the engine must report (Status::InvalidArgument).
      virtual const char* Name() const = 0;
      // Fold `operands` (oldest first) into `existing` (nullptr when the key
      // has no value) and write the result to `*new_value`.  Returning false
      // means the operands are corrupt; the engine must surface that as a
      // Status::Corruption from the read that hit them.
      virtual bool FullMerge(const Slice& key, const Slice* existing,
                             const std::vector<std::string>& operands,
                             std::string* new_value) const = 0;
      // Optional: fold two adjacent operands into one during compaction.
      // Returning false (the default) means "cannot", and both are kept.
      virtual bool PartialMerge(const Slice& key, const Slice& left,
                                const Slice& right, std::string* result) const;
    };
    }  // namespace leveldb

    // include/leveldb/options.h
    const MergeOperator* merge_operator = nullptr;   // in struct Options

    // include/leveldb/db.h
    virtual Status Merge(const WriteOptions& options, const Slice& key,
                         const Slice& value);

    // include/leveldb/write_batch.h
    void Merge(const Slice& key, const Slice& value);

`Merge(key, operand)` records an operand.  A read of `key` returns
`FullMerge(key, base, operands)` where `base` is the value of the most
recent `Put` below the oldest operand (or absent), and `operands` are every
operand above it in sequence order, oldest first.  A `Delete` or a range
tombstone below the operands removes the base, so `existing` is `nullptr`.

* `Get`, iteration and snapshot reads must all return merged values.
* Merging with no `merge_operator` configured is `Status::InvalidArgument`.
* Compaction may fold operands with `PartialMerge`, and may apply
  `FullMerge` once it reaches the base value, as long as reads are
  unaffected.

## 3. What stays the same

* Existing behaviour of `Put`, `Delete`, `Write`, `Get`, iterators,
  snapshots, `CompactRange`, `GetProperty` and the public headers.
* The engine must still open and read databases written by the untouched
  LevelDB 1.23 (the verifier has sealed ones, including a torn WAL and a
  live WAL to recover).  The reverse is not required: once you write range
  tombstones or operands, the untouched engine is not expected to read that
  database.
* Crash safety: every acknowledged write, including range deletes and
  operands, survives a crash at any point.
