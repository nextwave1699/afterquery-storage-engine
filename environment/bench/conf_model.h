// Conformance and recovery scenarios over LevelDB's public API.
//
// A scenario is a pure function of (seed, ops): ScenarioGen produces the
// operations, and it only looks at the model, never at the database, so the
// same seed always yields the same operations.  That is what lets a crashed
// run be checked afterwards: replay the generator up to the last
// acknowledged write and compare the recovered database with the model at
// that point (or one write later, the write that was in flight).
//
// Operations cover writes (single puts and deletes, mixed batches, batches
// that clear a key range), point reads, iterator walks that mix Seek, Next
// and Prev, full scans in both directions, snapshots, reopens and full or
// partial compactions.  Values carry their own key id and a version, so a
// value returned for the wrong key or from the wrong write never matches.
#ifndef LSMBENCH_CONF_MODEL_H_
#define LSMBENCH_CONF_MODEL_H_

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

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

// What the database should hold: key id -> value.
using ConfModel = std::map<uint64_t, std::string>;

struct ConfWrite {
  bool del;
  uint64_t id;
  std::string value;
};

enum ConfOpKind {
  kOpWrite,      // `writes`, applied as one WriteBatch (or a single Put/Delete)
  kOpGets,       // `count` point reads
  kOpWalk,       // an iterator: Seek to `id`, then `count` random Next/Prev
  kOpScan,       // full scan forwards and backwards
  kOpSnapTake,   // take a snapshot (releasing any previous one)
  kOpSnapCheck,  // read through the snapshot, then release it
  kOpReopen,     // close and open the database
  kOpCompact,    // CompactRange over [id, id + count), or everything if count == 0
};

struct ConfOp {
  ConfOpKind kind = kOpGets;
  std::vector<ConfWrite> writes;
  bool single = false;  // kOpWrite: use Put/Delete rather than Write
  uint64_t id = 0;
  uint64_t count = 0;
};

class ScenarioGen {
 public:
  ScenarioGen(uint64_t seed, uint64_t ops);

  uint64_t keys() const { return keys_; }

  // The next operation, given the model as it stands (range clears delete
  // whatever the model says is live).  False when the scenario is over.
  bool Next(const ConfModel& model, ConfOp* op);

  static void Apply(const ConfOp& op, ConfModel* model);

 private:
  ConfRng rng_;
  uint64_t keys_;
  uint64_t ops_;
  uint64_t done_ = 0;
  uint64_t version_ = 1;
  bool snap_open_ = false;
};

// The model after the first `writes` write operations of a scenario.
ConfModel ModelAfterWrites(uint64_t seed, uint64_t ops, uint64_t writes, bool* past_end);

void DestroyScenarioDir(const std::string& dir);

// Runs a scenario on a fresh database in `dir`.  With a non-empty `crash`
// spec (see bench_env.h) the process is killed at that Env event; `ack_fd`
// then receives the number of acknowledged write operations after each one.
bool RunScenario(const std::string& dir, uint64_t seed, uint64_t ops, const std::string& crash,
                 int ack_fd, std::string* error);

// Opens a crashed or finished database and checks that it holds the
// model after `acked` writes, or after `acked + 1` (the write in flight).
// `*recovered` is set to whichever it was.
bool CheckRecovered(const std::string& dir, uint64_t seed, uint64_t ops, uint64_t acked,
                    uint64_t* recovered, std::string* error);

}  // namespace lsmbench

#endif  // LSMBENCH_CONF_MODEL_H_
