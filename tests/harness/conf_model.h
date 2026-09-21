// The model a conformance scenario is checked against, the merge operator
// the engine is handed, and the scenario generator itself.
//
// A scenario is a pure function of (seed, ops).  The model is a plain
// std::map from key to value; every read the scenario makes is compared
// with it, and snapshots are checked against a copy taken when the
// snapshot was.  Values are a decimal integer followed by padding, and the
// merge operator adds those integers, so the model can reproduce exactly
// what a merged read must return.
#ifndef LSMBENCH_CONF_MODEL_H_
#define LSMBENCH_CONF_MODEL_H_

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "leveldb/db.h"
#include "leveldb/merge_operator.h"
#include "leveldb/options.h"
#include "leveldb/write_batch.h"

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

// Values are "<int><padding>"; the merge operator only looks at the int.
inline long long ConfParse(const leveldb::Slice& v) {
  return strtoll(std::string(v.data(), v.size()).c_str(), nullptr, 10);
}

inline std::string ConfValue(long long n, uint32_t pad) {
  return std::to_string(n) + std::string(pad, 'x');
}

// Adds the integers of the operands to the integer of the existing value.
class AddOperator : public leveldb::MergeOperator {
 public:
  const char* Name() const override { return "lsmbench.add"; }

  bool FullMerge(const leveldb::Slice& key, const leveldb::Slice* existing,
                 const std::vector<std::string>& operands,
                 std::string* new_value) const override {
    (void)key;
    long long v = existing == nullptr ? 0 : ConfParse(*existing);
    for (const std::string& op : operands) v += ConfParse(op);
    *new_value = std::to_string(v);
    return true;
  }

  bool PartialMerge(const leveldb::Slice& key, const leveldb::Slice& left,
                    const leveldb::Slice& right, std::string* result) const override {
    (void)key;
    *result = std::to_string(ConfParse(left) + ConfParse(right));
    return true;
  }
};

// What the database should contain: key id -> value.
using ConfModel = std::map<uint64_t, std::string>;

inline void ModelPut(ConfModel* m, uint64_t id, const std::string& v) { (*m)[id] = v; }
inline void ModelDelete(ConfModel* m, uint64_t id) { m->erase(id); }
inline void ModelDeleteRange(ConfModel* m, uint64_t lo, uint64_t hi) {
  if (lo >= hi) return;
  m->erase(m->lower_bound(lo), m->lower_bound(hi));
}
inline void ModelMerge(ConfModel* m, uint64_t id, long long delta) {
  auto it = m->find(id);
  long long base = it == m->end() ? 0 : strtoll(it->second.c_str(), nullptr, 10);
  (*m)[id] = std::to_string(base + delta);
}

void DestroyScenarioDir(const std::string& dir);

// The efficiency cases: "rangedel" deletes wide ranges of an already
// loaded database, "merge" records many operands for a few keys.  Both are
// measured from outside with the kernel's I/O counters, so what matters is
// what the engine writes and reads, not what it reports.
bool RunPerfCase(const std::string& dir, const std::string& which, uint64_t scale,
                 std::string* error);
bool RunScenario(const std::string& dir, uint64_t seed, uint64_t ops, bool verbose,
                 std::string* error);

}  // namespace lsmbench

#endif  // LSMBENCH_CONF_MODEL_H_
