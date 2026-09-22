// Conformance and recovery scenarios over LevelDB's public API, including
// the two extensions the task adds: range deletes (DB::DeleteRange,
// WriteBatch::DeleteRange) and merge operands (DB::Merge, WriteBatch::Merge,
// Options::merge_operator).
//
// A scenario is a pure function of (seed, ops, ext): ScenarioGen produces
// the operations, and it only looks at the model, never at the database, so
// the same seed always yields the same operations.  That is what lets a
// crashed run be checked afterwards: replay the generator up to the last
// acknowledged write and compare the recovered database with the model at
// that point (or one write later, the write that was in flight).
//
// Without `ext` the scenarios only use the stock API (that is what the
// untouched engine is run with).  With it, writes also include range
// deletes and merge operands, alone and inside batches around other writes,
// and reads check that they are honoured by Get, by iterators in both
// directions and through snapshots, across reopens and compactions.
//
// Values carry their own key id and a version, so a value returned for the
// wrong key or from the wrong write never matches.  The merge operator is
// an affine map x -> a*x + b (mod 2^61-1): operands do not commute, so an
// engine that applies them out of order, twice, or drops one returns a
// different value.
#ifndef LSMBENCH_CONF_MODEL_H_
#define LSMBENCH_CONF_MODEL_H_

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#ifndef CONF_PRISTINE
#include "leveldb/merge_operator.h"
#endif

namespace lsmbench {

inline uint64_t ConfMix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

struct ConfRng {
  uint64_t s;
  explicit ConfRng(uint64_t seed) : s(ConfMix(seed)) {}
  uint64_t Next() {
    s += 0x9E3779B97F4A7C15ull;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint64_t Below(uint64_t n) { return n == 0 ? 0 : Next() % n; }
  bool Chance(uint32_t percent) { return Below(100) < percent; }
};

inline std::string ConfKey(uint64_t id) {
  char buf[32];
  snprintf(buf, sizeof(buf), "key%08llu", static_cast<unsigned long long>(id));
  return std::string(buf);
}

// "<id>:<version>:" followed by padding; `pad` makes values of very
// different sizes so blocks and files fill unevenly.
inline std::string ConfValue(uint64_t id, uint64_t version, uint32_t pad) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%llu:%llu:", static_cast<unsigned long long>(id),
           static_cast<unsigned long long>(version));
  return std::string(buf) + std::string(pad, static_cast<char>('a' + version % 26));
}

// ---- the merge operator ---------------------------------------------------
//
// An operand is "<a>,<b>" (decimal), the map x -> a*x + b mod P.  A merged
// value is "M<x>:" (x in 19 digits) padded with 'm' to the length of the
// value it was merged into (so merged values stay as large as the values
// they replace).  The x
// of a plain value is a hash of it; with no value underneath, x starts from
// a hash of the key.  Partial merges compose two maps into one.
const uint64_t kAffineP = (1ull << 61) - 1;

inline uint64_t AffineMul(uint64_t a, uint64_t b) {
  unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
  uint64_t lo = static_cast<uint64_t>(r & kAffineP);
  uint64_t hi = static_cast<uint64_t>(r >> 61);
  uint64_t s = lo + hi;
  return s >= kAffineP ? s - kAffineP : s;
}

inline uint64_t AffineAdd(uint64_t a, uint64_t b) {
  uint64_t s = a + b;
  return s >= kAffineP ? s - kAffineP : s;
}

inline uint64_t AffineHash(const char* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h ^= static_cast<unsigned char>(p[i]);
    h *= 1099511628211ull;
  }
  return ConfMix(h) % kAffineP;
}

inline std::string AffineOperand(uint64_t a, uint64_t b) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%llu,%llu", static_cast<unsigned long long>(a),
           static_cast<unsigned long long>(b));
  return buf;
}

bool AffineParseOperand(const char* p, size_t n, uint64_t* a, uint64_t* b);
uint64_t AffineValueX(const std::string& key, const char* existing, size_t n, bool has_existing);
std::string AffineMerged(uint64_t x, size_t length);

// FullMerge on plain strings: what the database must return for `key`
// after `operands` (oldest first) on top of `existing` (nullptr: none).
std::string AffineFullMerge(const std::string& key, const std::string* existing,
                            const std::vector<std::string>& operands);

#ifndef CONF_PRISTINE
class AffineOperator : public leveldb::MergeOperator {
 public:
  const char* Name() const override { return "lsmbench.affine"; }
  bool FullMerge(const leveldb::Slice& key, const leveldb::Slice* existing,
                 const std::vector<std::string>& operands,
                 std::string* new_value) const override;
  bool PartialMerge(const leveldb::Slice& key, const leveldb::Slice& left,
                    const leveldb::Slice& right, std::string* result) const override;
};
#endif

// ---- scenarios --------------------------------------------------------------

// What the database should hold: key id -> value.
using ConfModel = std::map<uint64_t, std::string>;

enum ConfWriteKind { kWPut, kWDelete, kWMerge, kWDeleteRange };

struct ConfWrite {
  ConfWriteKind kind;
  uint64_t id;          // kWDeleteRange: begin
  uint64_t end;         // kWDeleteRange: end (exclusive; may be <= begin)
  std::string value;    // kWPut: value, kWMerge: operand
};

enum ConfOpKind {
  kOpWrite,      // `writes`, applied as one WriteBatch (or a single call)
  kOpGets,       // `count` point reads
  kOpWalk,       // an iterator: Seek to `id`, then `count` random Next/Prev
  kOpScan,       // full scan forwards and backwards
  kOpSnapTake,   // take a snapshot (up to three are held at once)
  kOpSnapCheck,  // read through snapshot `id`, then release it
  kOpReopen,     // close and open the database
  kOpCompact,    // CompactRange over [id, id + count), or everything if count == 0
};

struct ConfOp {
  ConfOpKind kind = kOpGets;
  std::vector<ConfWrite> writes;
  bool single = false;  // kOpWrite: use Put/Delete/Merge/DeleteRange rather than Write
  uint64_t id = 0;
  uint64_t count = 0;
};

// Engine options a scenario runs with; derived from the seed so a crashed
// run is reopened the same way.
struct ConfShape {
  uint64_t keys;
  bool bloom;
  bool reuse_logs;
  bool small_cache;
  uint32_t write_buffer;
  uint32_t max_file;
  uint32_t block_size;
};

class ScenarioGen {
 public:
  ScenarioGen(uint64_t seed, uint64_t ops, bool ext);

  uint64_t keys() const { return shape_.keys; }
  const ConfShape& shape() const { return shape_; }

  // The next operation, given the model as it stands (range clears delete
  // whatever the model says is live).  False when the scenario is over.
  bool Next(const ConfModel& model, ConfOp* op);

  static void Apply(const ConfOp& op, ConfModel* model);

 private:
  void RandomWrite(ConfWrite* w, uint64_t id, bool allow_merge);

  ConfRng rng_;
  ConfShape shape_;
  uint64_t ops_;
  bool ext_;
  uint64_t done_ = 0;
  uint64_t version_ = 1;
  int snaps_open_ = 0;
};

ConfShape ShapeFor(uint64_t seed);

// The model after the first `writes` write operations of a scenario.
ConfModel ModelAfterWrites(uint64_t seed, uint64_t ops, bool ext, uint64_t writes, bool* past_end);

void DestroyScenarioDir(const std::string& dir);

// Runs a scenario on a fresh database in `dir`.  With a non-empty `crash`
// spec (see bench_env.h) the process is killed at that Env event; `ack_fd`
// then receives the number of acknowledged write operations after each one.
bool RunScenario(const std::string& dir, uint64_t seed, uint64_t ops, bool ext,
                 const std::string& crash, int ack_fd, std::string* error);

// Opens a crashed or finished database and checks that it holds the
// model after `acked` writes, or after `acked + 1` (the write in flight).
// `*recovered` is set to whichever it was.
bool CheckRecovered(const std::string& dir, uint64_t seed, uint64_t ops, bool ext,
                    uint64_t acked, uint64_t* recovered, std::string* error);

#ifndef CONF_PRISTINE
// Fixed checks of the extension API that random scenarios do not reach:
// WriteBatch::Iterate/Append with the new records, what happens without a
// merge operator, empty and reversed ranges.  Prints one line per check.
bool RunApiChecks(const std::string& dir, int* passed, int* total);

// The efficiency cases: "rangedel" deletes most of a loaded database with
// a few hundred range deletes, "manyranges" interleaves 100,000 small range
// deletes with reads, "merge" records many operands on loaded keys.  Prints "METRIC <name> <value>"
// lines (kernel I/O counters of phases, table bytes on disk) and checks
// every read against the model.
bool RunPerfCase(const std::string& dir, const std::string& which, double scale,
                 std::string* error);
#endif

}  // namespace lsmbench

#endif  // LSMBENCH_CONF_MODEL_H_
