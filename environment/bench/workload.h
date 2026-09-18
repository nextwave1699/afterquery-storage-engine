// The fixed benchmark workload and the in-memory model used to check every
// read against.
//
// Everything is a pure function of (seed, scale): the sequence of steps, the
// key of every operation, the length and bytes of every value.  Keys are
// 16-digit zero-padded decimal ids; a value is derived from (seed, id, w)
// where w is the global write counter at the time of the write, so the model
// only has to remember one 32-bit number per id and can regenerate the value
// expected from any read.
//
// There are several workloads (--workload). `mixed` is the original one and
// the only one the crash, compatibility and differential stages use; its
// steps and values must never change, or the sealed fixtures stop matching.
// The others are scored alongside it and are described at WorkloadKind.
//
// Phases of `mixed` (sizes scale linearly with `scale`):
//   A  load        sequential inserts of ids [0, N_A)
//   B  insert      random-order inserts of ids [N_A, N_A+N_B), some point reads
//   C  update      updates, 75% inside one contiguous hot key range and 25%
//                  uniform, scans, snapshot reads
//   D  delete      deletes: a contiguous cold range purged in key order plus
//                  uniform deletes; reads of deleted and live keys
//   E  mixed       updates concentrated in a hot window that slides through
//                  the key space, inserts of new ids, deletes, gets, scans,
//                  snapshots
//   F  read        point reads, forward and reverse scans, one full scan
//                  (run by `lsmbench read` as a separate process)
//   X  extension   unbounded mixed updates/deletes/reads over existing ids,
//                  produced only when a checker asks to continue past E
#ifndef LSMBENCH_WORKLOAD_H_
#define LSMBENCH_WORKLOAD_H_

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lsmbench {

static const uint32_t kKeyLen = 16;
static const uint32_t kMinValue = 48;
static const uint32_t kMaxValue = 400;
static const uint32_t kDeletedBit = 0x80000000u;
static const uint32_t kMaxAnyValue = 8192;  // largest value any workload writes

//   mixed    phases A-E below (the original workload)
//   uniform  load, then uniform random overwrites of every key with a few
//            deletes and snapshots: the classic worst case for leveling
//   series   ids grow like timestamps; 12% of writes are late corrections to
//            the most recent ids, and the oldest ids are purged in key order
//            to keep a bounded live window (TTL-style range deletes)
//   blob     1-6 KB values; load, then updates where 70% hit a scattered
//            hot set of 8% of the keys
enum WorkloadKind { kMixed = 0, kUniform, kSeries, kBlob };

inline bool ParseWorkloadKind(const std::string& name, WorkloadKind* k) {
  if (name == "mixed") *k = kMixed;
  else if (name == "uniform") *k = kUniform;
  else if (name == "series") *k = kSeries;
  else if (name == "blob") *k = kBlob;
  else return false;
  return true;
}

// Value lengths depend on the workload, so they are a process-wide setting
// that the Workload constructor installs before anything is generated.
struct ValueProfile {
  uint32_t min_len, max_len;
};
inline ValueProfile& CurrentValueProfile() {
  static ValueProfile p{kMinValue, kMaxValue};
  return p;
}

inline uint64_t Mix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(Mix64(seed)) {}
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

inline std::string KeyOf(uint64_t id) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%016llu", static_cast<unsigned long long>(id));
  return std::string(buf, kKeyLen);
}

// Returns false if `key` is not a well-formed benchmark key.
inline bool IdOf(const char* data, size_t n, uint64_t* id) {
  if (n != kKeyLen) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (data[i] < '0' || data[i] > '9') return false;
    v = v * 10 + static_cast<uint64_t>(data[i] - '0');
  }
  *id = v;
  return true;
}

struct ValueGen {
  static uint64_t Salt(uint64_t seed, uint64_t id, uint32_t w) {
    return Mix64(Mix64(seed ^ 0x5157ull) + Mix64(id * 0x100000001B3ull + w));
  }
  static uint32_t Length(uint64_t seed, uint64_t id, uint32_t w) {
    const ValueProfile& p = CurrentValueProfile();
    return p.min_len + static_cast<uint32_t>(Salt(seed, id, w) % (p.max_len - p.min_len + 1));
  }
  static void Fill(uint64_t seed, uint64_t id, uint32_t w, char* out, uint32_t len) {
    Rng r(Salt(seed, id, w) ^ 0xABCDull);
    uint32_t i = 0;
    while (i + 8 <= len) {
      uint64_t x = r.Next();
      memcpy(out + i, &x, 8);
      i += 8;
    }
    if (i < len) {
      uint64_t x = r.Next();
      memcpy(out + i, &x, len - i);
    }
    // Keep one recognisable byte so a dump is easy to eyeball.
    out[0] = 'v';
  }
  static std::string Make(uint64_t seed, uint64_t id, uint32_t w) {
    uint32_t len = Length(seed, id, w);
    std::string s(len, '\0');
    Fill(seed, id, w, &s[0], len);
    return s;
  }
  static bool Equals(uint64_t seed, uint64_t id, uint32_t w, const char* data, size_t n) {
    uint32_t len = Length(seed, id, w);
    if (n != len) return false;
    char buf[kMaxAnyValue];
    Fill(seed, id, w, buf, len);
    return memcmp(buf, data, len) == 0;
  }
};

// The expected state of the database.
struct Model {
  uint64_t seed;
  std::vector<uint32_t> ver;  // per id: 0 = never written; w | kDeletedBit = deleted at w
  uint32_t wseq = 0;
  uint64_t logical_bytes = 0;  // sum over writes of key + value bytes (key only for deletes)
  uint64_t puts = 0;
  uint64_t dels = 0;

  Model(uint64_t s, uint64_t universe) : seed(s), ver(universe, 0) {}

  bool Live(uint64_t id) const {
    return id < ver.size() && ver[id] != 0 && (ver[id] & kDeletedBit) == 0;
  }
  // Smallest live id >= from, or ver.size() if none.
  uint64_t NextLive(uint64_t from) const {
    uint64_t i = from;
    while (i < ver.size() && !Live(i)) i++;
    return i;
  }
  // Largest live id <= from, or UINT64_MAX if none.
  uint64_t PrevLive(uint64_t from) const {
    if (from >= ver.size()) from = ver.size() - 1;
    int64_t i = static_cast<int64_t>(from);
    while (i >= 0 && !Live(static_cast<uint64_t>(i))) i--;
    return i < 0 ? UINT64_MAX : static_cast<uint64_t>(i);
  }
  void Put(uint64_t id) {
    wseq++;
    ver[id] = wseq;
    logical_bytes += kKeyLen + ValueGen::Length(seed, id, wseq);
    puts++;
  }
  void Del(uint64_t id) {
    wseq++;
    ver[id] = wseq | kDeletedBit;
    logical_bytes += kKeyLen;
    dels++;
  }
};

enum StepKind {
  kStepBatch = 0,    // ops: write batch
  kStepGet,          // id
  kStepScan,         // id = start, count
  kStepRScan,        // id = start (reverse), count
  kStepSnapTake,     // ids: sample to remember
  kStepSnapCheck,    // read the remembered sample through the snapshot, release it
  kStepFullScan,     // forward + reverse scan of everything
  kStepPhase,        // phase marker (name in `phase`)
};

struct Op {
  bool del;
  uint64_t id;
};

struct Step {
  StepKind kind;
  std::vector<Op> ops;      // kStepBatch
  std::vector<uint64_t> ids;  // kStepSnapTake
  uint64_t id = 0;
  uint32_t count = 0;
  char phase = '?';
};

struct WorkloadShape {
  uint64_t n_load, n_insert, n_hot, n_updates, n_deletes, n_mixed, n_mixed_new;
  uint64_t universe;      // ids that may ever be written
  uint64_t read_universe; // ids that may be read (includes never-written ids)
  uint64_t f_gets, f_scans, f_scan_len, f_rscans, f_rscan_len;
  // series only: live window kept after purging, and how far back late
  // corrections reach
  uint64_t window = 0, lag = 0;

  WorkloadShape(double scale, WorkloadKind kind) {
    auto sc = [&](double base, uint64_t min) {
      uint64_t v = static_cast<uint64_t>(base * scale + 0.5);
      return v < min ? min : v;
    };
    if (kind != kMixed) {
      InitOther(scale, kind);
      return;
    }
    n_load = sc(240000, 200);
    n_insert = sc(240000, 200);
    n_hot = sc(30000, 64);
    n_updates = sc(320000, 200);
    n_deletes = sc(120000, 100);
    n_mixed = sc(240000, 200);
    n_mixed_new = sc(120000, 100);
    universe = n_load + n_insert + n_mixed_new;
    read_universe = universe + universe / 16 + 64;
    f_gets = sc(20000, 200);
    f_scans = sc(200, 8);
    f_scan_len = 500;
    f_rscans = sc(20, 4);
    f_rscan_len = 200;
  }

 private:
  // n_load: keys loaded first (uniform, blob); n_updates: operations of the
  // main phase (series: ids appended); n_hot: blob's hot set.
  void InitOther(double scale, WorkloadKind kind) {
    auto sc = [&](double base, uint64_t min) {
      uint64_t v = static_cast<uint64_t>(base * scale + 0.5);
      return v < min ? min : v;
    };
    n_insert = n_deletes = n_mixed = n_mixed_new = 0;
    n_hot = 0;
    if (kind == kUniform) {
      n_load = sc(200000, 200);
      n_updates = sc(600000, 400);
      universe = n_load;
    } else if (kind == kSeries) {
      n_load = 0;
      n_updates = sc(700000, 800);
      universe = n_updates;
      window = sc(180000, 200);
      lag = sc(20000, 50);
    } else {
      n_load = sc(30000, 100);
      n_updates = sc(30000, 100);
      n_hot = n_load * 8 / 100 + 1;
      universe = n_load;
    }
    read_universe = universe + universe / 16 + 64;
    f_gets = sc(20000, 200);
    f_scans = sc(200, 8);
    f_scan_len = kind == kBlob ? 50 : 500;
    f_rscans = sc(20, 4);
    f_rscan_len = kind == kBlob ? 20 : 200;
  }
};

// Generates the steps of the write phases (A..E) and, separately, phase F.
class Workload {
 public:
  Workload(uint64_t seed, double scale, WorkloadKind kind = kMixed)
      : seed_(seed), kind_(kind), shape_(scale, kind), rng_(seed ^ 0x77ull), batches_(0) {
    ValueProfile& vp = CurrentValueProfile();
    vp = kind == kBlob ? ValueProfile{1024, 6144} : ValueProfile{kMinValue, kMaxValue};
    Rng hr(seed ^ 0x407ull);
    // Phase C's hot range: n_hot consecutive ids somewhere in the loaded space.
    hot_lo_ = hr.Below(shape_.n_load + shape_.n_insert - shape_.n_hot);
    // Phase D's purged range: 60% of the deletes, in key order.
    del_range_len_ = shape_.n_deletes * 6 / 10;
    del_lo_ = hr.Below(shape_.n_load + shape_.n_insert - del_range_len_);
    del_next_ = del_lo_;
    max_written_ = 0;
    pos_ = 0;
    sub_ = 0;
    perm_key_ = Mix64(seed ^ 0x9999ull);
  }

  const WorkloadShape& shape() const { return shape_; }
  WorkloadKind kind() const { return kind_; }
  uint64_t seed() const { return seed_; }
  uint64_t batches() const { return batches_; }

  // After phase E, keep producing extension steps (phase X) instead of
  // reporting the end of the workload.
  void AllowExtension() { allow_extension_ = true; }

  // Next step of phases A..E.  Returns false when they are exhausted.
  bool Next(Step* st) {
    st->ops.clear();
    st->ids.clear();
    st->count = 0;
    st->id = 0;
    if (kind_ != kMixed) return NextOther(st);
    while (true) {
      switch (pos_) {
        case 0:  // phase A marker
          pos_ = 1; sub_ = 0;
          return Marker(st, 'A');
        case 1:
          if (sub_ >= shape_.n_load) { pos_ = 2; sub_ = 0; break; }
          return LoadStep(st);
        case 2:
          pos_ = 3; sub_ = 0;
          return Marker(st, 'B');
        case 3:
          if (sub_ >= shape_.n_insert) { pos_ = 4; sub_ = 0; break; }
          return InsertStep(st);
        case 4:
          pos_ = 5; sub_ = 0;
          return Marker(st, 'C');
        case 5:
          if (sub_ >= shape_.n_updates) {
            if (snap_pending_) { snap_pending_ = false; return SnapCheck(st, 'C'); }
            pos_ = 6; sub_ = 0; break;
          }
          return UpdateStep(st);
        case 6:
          pos_ = 7; sub_ = 0;
          return Marker(st, 'D');
        case 7:
          if (sub_ >= shape_.n_deletes) { pos_ = 8; sub_ = 0; break; }
          return DeleteStep(st);
        case 8:
          pos_ = 9; sub_ = 0; mixed_new_next_ = shape_.n_load + shape_.n_insert;
          snap_taken_at_ = UINT64_MAX;
          return Marker(st, 'E');
        case 9:
          if (sub_ >= shape_.n_mixed) {
            if (snap_pending_) { snap_pending_ = false; return SnapCheck(st, 'E'); }
            pos_ = 10; sub_ = 0; break;
          }
          return MixedStep(st);
        case 10:
          if (!allow_extension_) return false;
          pos_ = 11;
          return Marker(st, 'X');
        default:
          if (!allow_extension_) return false;
          return ExtensionStep(st);
      }
    }
  }

  // Phase F steps: independent of the write-phase generator state.
  void PhaseF(std::vector<Step>* out) const {
    out->clear();
    Rng r(seed_ ^ 0xF00Dull);
    Step m;
    m.kind = kStepPhase;
    m.phase = 'F';
    out->push_back(m);
    // series reads mostly hit the live window at the end of the id space
    const uint64_t recent = kind_ == kSeries ? shape_.window : shape_.universe;
    for (uint64_t i = 0; i < shape_.f_gets; i++) {
      Step s;
      s.kind = kStepGet;
      s.phase = 'F';
      if (kind_ == kSeries && r.Chance(85)) {
        s.id = shape_.universe - 1 - r.Below(recent);
      } else {
        s.id = r.Chance(85) ? r.Below(shape_.universe) : r.Below(shape_.read_universe);
      }
      out->push_back(s);
    }
    for (uint64_t i = 0; i < shape_.f_scans; i++) {
      Step s;
      s.kind = kStepScan;
      s.phase = 'F';
      s.id = r.Below(shape_.read_universe);
      s.count = shape_.f_scan_len;
      out->push_back(s);
    }
    for (uint64_t i = 0; i < shape_.f_rscans; i++) {
      Step s;
      s.kind = kStepRScan;
      s.phase = 'F';
      s.id = r.Below(shape_.read_universe);
      s.count = shape_.f_rscan_len;
      out->push_back(s);
    }
    Step f;
    f.kind = kStepFullScan;
    f.phase = 'F';
    out->push_back(f);
  }

 private:
  bool Marker(Step* st, char phase) {
    st->kind = kStepPhase;
    st->phase = phase;
    return true;
  }
  void Batch(Step* st, char phase) {
    st->kind = kStepBatch;
    st->phase = phase;
    batches_++;
  }
  void AddPut(Step* st, uint64_t id) {
    st->ops.push_back(Op{false, id});
    if (id + 1 > max_written_) max_written_ = id + 1;
  }
  void AddDel(Step* st, uint64_t id) { st->ops.push_back(Op{true, id}); }

  uint64_t ExistingId() {  // uniform over everything written so far
    return rng_.Below(max_written_ == 0 ? 1 : max_written_);
  }
  uint64_t HotId() { return hot_lo_ + rng_.Below(shape_.n_hot); }
  // Phase E's hot window slides from hot_lo_ across the key space as the
  // phase progresses.
  uint64_t WindowId() {
    uint64_t span = shape_.n_load + shape_.n_insert;
    uint64_t start = (hot_lo_ + (sub_ * span) / shape_.n_mixed) % span;
    return (start + rng_.Below(shape_.n_hot / 2 + 1)) % span;
  }
  uint64_t ReadId() {
    return rng_.Chance(85) ? ExistingId() : rng_.Below(shape_.read_universe);
  }
  bool Get(Step* st, char phase) {
    st->kind = kStepGet;
    st->phase = phase;
    st->id = ReadId();
    return true;
  }
  bool Scan(Step* st, char phase, uint32_t lo, uint32_t hi) {
    st->kind = kStepScan;
    st->phase = phase;
    st->id = rng_.Below(shape_.read_universe);
    st->count = lo + static_cast<uint32_t>(rng_.Below(hi - lo + 1));
    return true;
  }
  bool SnapTake(Step* st, char phase) {
    st->kind = kStepSnapTake;
    st->phase = phase;
    for (int i = 0; i < 64; i++) {
      st->ids.push_back((i & 1) ? HotId() : ExistingId());
    }
    return true;
  }
  bool SnapCheck(Step* st, char phase) {
    st->kind = kStepSnapCheck;
    st->phase = phase;
    return true;
  }

  // A: sequential inserts, 8 per batch.
  bool LoadStep(Step* st) {
    Batch(st, 'A');
    for (int i = 0; i < 8 && sub_ < shape_.n_load; i++) {
      AddPut(st, sub_);
      sub_++;
    }
    return true;
  }

  // B: inserts of ids [n_load, n_load+n_insert) in a pseudo-random order
  // (a bijective permutation of the range), 1-4 per batch, a point read
  // every 16 batches.
  uint64_t Permute(uint64_t i, uint64_t n) const {
    // Cycle-walking Feistel permutation over [0, n): a 4-round Feistel
    // network on an even number of bits covering n, re-applied until the
    // result lands inside [0, n).  A bijection on [0, n).
    uint64_t bits = 2;
    while ((1ull << bits) < n) bits += 2;
    const uint64_t half = bits / 2;
    const uint64_t hm = (1ull << half) - 1;
    uint64_t x = i;
    do {
      uint64_t l = x & hm, r = (x >> half) & hm;
      for (uint64_t round = 0; round < 4; round++) {
        uint64_t f = Mix64(r + perm_key_ + round * 0x9E37ull) & hm;
        uint64_t nl = r, nr = l ^ f;
        l = nl;
        r = nr;
      }
      x = (r << half) | l;
    } while (x >= n);
    return x;
  }
  bool InsertStep(Step* st) {
    if (sub_ > 0 && insert_batches_ % 16 == 0 && !did_read_) {
      did_read_ = true;
      return Get(st, 'B');
    }
    did_read_ = false;
    Batch(st, 'B');
    insert_batches_++;
    int n = 1 + static_cast<int>(rng_.Below(4));
    for (int i = 0; i < n && sub_ < shape_.n_insert; i++) {
      AddPut(st, shape_.n_load + Permute(sub_, shape_.n_insert));
      sub_++;
    }
    return true;
  }

  // C: updates, 75% inside the hot range, 1-8 per batch; a get every 8 batches,
  // a scan every 100, a snapshot taken every 3000 and checked 1500 later.
  bool UpdateStep(Step* st) {
    if (snap_pending_ && update_batches_ >= snap_check_at_) {
      snap_pending_ = false;
      return SnapCheck(st, 'C');
    }
    if (update_batches_ > 0 && update_batches_ % 3000 == 0 && !snap_pending_ && snap_taken_at_ != update_batches_) {
      snap_taken_at_ = update_batches_;
      snap_pending_ = true;
      snap_check_at_ = update_batches_ + 1500;
      return SnapTake(st, 'C');
    }
    if (update_batches_ > 0 && update_batches_ % 100 == 0 && pending_read_ == 0) {
      pending_read_ = 1;
      return Scan(st, 'C', 100, 300);
    }
    if (update_batches_ > 0 && update_batches_ % 8 == 0 && pending_read_ < 2) {
      pending_read_ = 2;
      return Get(st, 'C');
    }
    pending_read_ = 0;
    Batch(st, 'C');
    update_batches_++;
    int n = 1 + static_cast<int>(rng_.Below(8));
    for (int i = 0; i < n && sub_ < shape_.n_updates; i++) {
      uint64_t id = rng_.Chance(75) ? HotId() : ExistingId();
      AddPut(st, id);
      sub_++;
    }
    return true;
  }

  // D: deletes, 1-4 per batch: 60% purge a contiguous range in key order,
  // the rest hit random ids; a get every 4 batches, half of them of an id
  // deleted in this phase.
  bool DeleteStep(Step* st) {
    if (delete_batches_ > 0 && delete_batches_ % 4 == 0 && pending_read_ == 0) {
      pending_read_ = 1;
      st->kind = kStepGet;
      st->phase = 'D';
      st->id = (rng_.Chance(50) && !deleted_.empty()) ? deleted_[rng_.Below(deleted_.size())]
                                                      : ReadId();
      return true;
    }
    pending_read_ = 0;
    Batch(st, 'D');
    delete_batches_++;
    int n = 1 + static_cast<int>(rng_.Below(4));
    bool range_batch = rng_.Chance(60) && del_next_ < del_lo_ + del_range_len_;
    for (int i = 0; i < n && sub_ < shape_.n_deletes; i++) {
      uint64_t id = range_batch ? del_next_++ : ExistingId();
      AddDel(st, id);
      if (deleted_.size() < 4096) deleted_.push_back(id);
      else deleted_[rng_.Below(deleted_.size())] = id;
      sub_++;
    }
    return true;
  }

  // E: mixed.  Per step: 20% get, 5% scan, else a batch of 1-6 ops that are
  // 60% updates (two thirds of them in the sliding hot window), 25% inserts
  // of new ids, 15% deletes; snapshots as in C.
  bool MixedStep(Step* st) {
    if (snap_pending_ && mixed_batches_ >= snap_check_at_) {
      snap_pending_ = false;
      return SnapCheck(st, 'E');
    }
    if (mixed_batches_ > 0 && mixed_batches_ % 3000 == 0 && !snap_pending_ && snap_taken_at_ != mixed_batches_) {
      snap_taken_at_ = mixed_batches_;
      snap_pending_ = true;
      snap_check_at_ = mixed_batches_ + 1500;
      return SnapTake(st, 'E');
    }
    uint64_t roll = rng_.Below(100);
    if (roll < 20) return Get(st, 'E');
    if (roll < 25) return Scan(st, 'E', 32, 256);
    Batch(st, 'E');
    mixed_batches_++;
    int n = 1 + static_cast<int>(rng_.Below(6));
    for (int i = 0; i < n && sub_ < shape_.n_mixed; i++) {
      uint64_t r = rng_.Below(100);
      if (r < 60) {
        AddPut(st, rng_.Chance(67) ? WindowId() : ExistingId());
      } else if (r < 85 && mixed_new_next_ < shape_.universe) {
        AddPut(st, mixed_new_next_++);
      } else {
        AddDel(st, ExistingId());
      }
      sub_++;
    }
    return true;
  }

  // X: like E without new ids and without snapshots; unbounded.
  bool ExtensionStep(Step* st) {
    uint64_t roll = rng_.Below(100);
    if (roll < 20) return Get(st, 'X');
    if (roll < 25) return Scan(st, 'X', 32, 256);
    Batch(st, 'X');
    int n = 1 + static_cast<int>(rng_.Below(6));
    for (int i = 0; i < n; i++) {
      if (rng_.Chance(80)) {
        AddPut(st, rng_.Chance(50) ? WindowId() : ExistingId());
      } else {
        AddDel(st, ExistingId());
      }
      sub_++;
    }
    return true;
  }

  // uniform, series and blob: an optional load phase 'A', then the main
  // phase 'B'; with AllowExtension, 'X' keeps going over existing ids.
  bool NextOther(Step* st) {
    while (true) {
      switch (pos_) {
        case 0:
          pos_ = 1; sub_ = 0;
          if (shape_.n_load == 0) break;
          return Marker(st, 'A');
        case 1:
          if (sub_ >= shape_.n_load) { pos_ = 2; sub_ = 0; break; }
          return OtherLoadStep(st);
        case 2:
          pos_ = 3; sub_ = 0;
          return Marker(st, 'B');
        case 3:
          if (sub_ >= shape_.n_updates) {
            if (snap_pending_) { snap_pending_ = false; return SnapCheck(st, 'B'); }
            pos_ = 4; break;
          }
          return OtherMainStep(st, 'B');
        case 4:
          if (!allow_extension_) return false;
          pos_ = 5;
          return Marker(st, 'X');
        default:
          if (!allow_extension_) return false;
          return OtherMainStep(st, 'X');
      }
    }
  }

  bool OtherLoadStep(Step* st) {
    Batch(st, 'A');
    int per = kind_ == kBlob ? 2 : 8;
    for (int i = 0; i < per && sub_ < shape_.n_load; i++) {
      AddPut(st, sub_);
      sub_++;
    }
    return true;
  }

  // A blob hot id: one of n_hot ids scattered over the whole key space.
  uint64_t BlobHotId() {
    return Mix64(seed_ ^ 0xB10Bull ^ rng_.Below(shape_.n_hot)) % shape_.n_load;
  }
  uint64_t SeriesRecentId() {
    uint64_t live = head_ - purge_next_;
    return head_ - 1 - rng_.Below(live == 0 ? 1 : live);
  }

  bool OtherMainStep(Step* st, char phase) {
    const bool ext = phase == 'X';
    if (kind_ == kUniform && !ext) {
      if (snap_pending_ && main_batches_ >= snap_check_at_) {
        snap_pending_ = false;
        return SnapCheck(st, phase);
      }
      if (main_batches_ > 0 && main_batches_ % 3000 == 0 && !snap_pending_ && snap_taken_at_ != main_batches_) {
        snap_taken_at_ = main_batches_;
        snap_pending_ = true;
        snap_check_at_ = main_batches_ + 1500;
        st->kind = kStepSnapTake;
        st->phase = phase;
        for (int i = 0; i < 64; i++) st->ids.push_back(ExistingId());
        return true;
      }
    }
    uint64_t roll = rng_.Below(100);
    if (kind_ == kSeries) {
      if (head_ > purge_next_ && roll < 10) {
        st->kind = kStepGet;
        st->phase = phase;
        st->id = rng_.Chance(90) ? SeriesRecentId() : rng_.Below(shape_.read_universe);
        return true;
      }
      if (head_ > purge_next_ && roll < 11) {
        st->kind = kStepScan;
        st->phase = phase;
        st->id = SeriesRecentId();
        st->count = 100 + static_cast<uint32_t>(rng_.Below(401));
        return true;
      }
      Batch(st, phase);
      main_batches_++;
      for (int i = 0; i < 8; i++) {
        bool late = head_ > 0 && (ext || head_ >= shape_.universe || rng_.Chance(12));
        if (late) {
          uint64_t back = head_ - purge_next_;
          if (back > shape_.lag) back = shape_.lag;
          if (back == 0) continue;
          AddPut(st, head_ - 1 - rng_.Below(back));
        } else {
          AddPut(st, head_++);
          if (!ext) sub_++;
        }
      }
      // keep the live window bounded: purge the oldest ids in key order
      while (head_ - purge_next_ > shape_.window && st->ops.size() < 24) {
        AddDel(st, purge_next_++);
      }
      return true;
    }
    uint32_t get_pct = kind_ == kBlob ? 10 : 8;
    if (roll < get_pct) return Get(st, phase);
    if (roll < get_pct + 1) {
      return kind_ == kBlob ? Scan(st, phase, 8, 32) : Scan(st, phase, 50, 200);
    }
    Batch(st, phase);
    main_batches_++;
    int n = kind_ == kBlob ? 1 + static_cast<int>(rng_.Below(2)) : 1 + static_cast<int>(rng_.Below(8));
    for (int i = 0; i < n && (ext || sub_ < shape_.n_updates); i++) {
      uint64_t r = rng_.Below(100);
      if (r < 5) {
        AddDel(st, ExistingId());
      } else if (kind_ == kBlob && r < 75) {
        AddPut(st, BlobHotId());
      } else {
        AddPut(st, ExistingId());
      }
      if (!ext) sub_++;
    }
    return true;
  }

  const uint64_t seed_;
  const WorkloadKind kind_;
  const WorkloadShape shape_;
  Rng rng_;
  uint64_t batches_;
  uint64_t hot_lo_ = 0;
  uint64_t del_lo_ = 0, del_range_len_ = 0, del_next_ = 0;
  std::vector<uint64_t> deleted_;
  uint64_t max_written_;
  uint64_t perm_key_;
  int pos_;
  uint64_t sub_;
  uint64_t mixed_new_next_ = 0;
  uint64_t head_ = 0, purge_next_ = 0, main_batches_ = 0;  // uniform/series/blob
  uint64_t insert_batches_ = 0, update_batches_ = 0, delete_batches_ = 0, mixed_batches_ = 0;
  bool did_read_ = false;
  int pending_read_ = 0;
  bool snap_pending_ = false;
  uint64_t snap_check_at_ = 0, snap_taken_at_ = UINT64_MAX;
  bool allow_extension_ = false;
};

}  // namespace lsmbench

#endif  // LSMBENCH_WORKLOAD_H_
