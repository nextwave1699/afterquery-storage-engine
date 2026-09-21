#include "conf_model.h"

#include "bench_env.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>

namespace lsmbench {
namespace {

const uint32_t kMaxPad = 120;

struct Scenario {
  leveldb::DB* db = nullptr;
  leveldb::Options options;
  // The harness supplies the Env, so every byte the engine reads or writes
  // goes through code the verifier owns (and none of it through mmap).
  BenchEnv env;
  AddOperator merge_op;
  std::string dir;
  ConfModel model;
  uint64_t keys = 0;
  long long next_int = 1;
  std::string error;

  bool Fail(const std::string& what) {
    if (error.empty()) error = what;
    return false;
  }
};

std::string Describe(uint64_t id) { return ConfKey(id); }

// Every key the model holds, in key order, must be exactly what a forward
// iteration returns; the same backwards.
bool CheckIteration(Scenario* s, const leveldb::ReadOptions& ro, const ConfModel& model,
                    const char* where) {
  std::unique_ptr<leveldb::Iterator> it(s->db->NewIterator(ro));
  auto expect = model.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (expect == model.end()) {
      return s->Fail(std::string(where) + ": iterator returned extra key " +
                     it->key().ToString());
    }
    if (it->key().ToString() != Describe(expect->first)) {
      return s->Fail(std::string(where) + ": iterator at " + it->key().ToString() +
                     ", expected " + Describe(expect->first));
    }
    if (it->value().ToString() != expect->second) {
      return s->Fail(std::string(where) + ": value of " + it->key().ToString() + " is '" +
                     it->value().ToString() + "', expected '" + expect->second + "'");
    }
    ++expect;
  }
  if (!it->status().ok()) return s->Fail(std::string(where) + ": " + it->status().ToString());
  if (expect != model.end()) {
    return s->Fail(std::string(where) + ": iterator stopped before " +
                   Describe(expect->first));
  }
  auto rexpect = model.rbegin();
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    if (rexpect == model.rend()) {
      return s->Fail(std::string(where) + ": reverse iterator returned extra key " +
                     it->key().ToString());
    }
    if (it->key().ToString() != Describe(rexpect->first) ||
        it->value().ToString() != rexpect->second) {
      return s->Fail(std::string(where) + ": reverse iterator at " + it->key().ToString() +
                     ", expected " + Describe(rexpect->first));
    }
    ++rexpect;
  }
  if (!it->status().ok()) return s->Fail(std::string(where) + ": " + it->status().ToString());
  if (rexpect != model.rend()) {
    return s->Fail(std::string(where) + ": reverse iterator stopped before " +
                   Describe(rexpect->first));
  }
  return true;
}

// Point reads of a sample of ids, present and absent.
bool CheckGets(Scenario* s, const leveldb::ReadOptions& ro, const ConfModel& model,
               ConfRng* rng, uint64_t n, const char* where) {
  for (uint64_t i = 0; i < n; i++) {
    uint64_t id = rng->Below(s->keys);
    std::string got;
    leveldb::Status st = s->db->Get(ro, Describe(id), &got);
    auto it = model.find(id);
    if (it == model.end()) {
      if (st.IsNotFound()) continue;
      if (!st.ok()) return s->Fail(std::string(where) + ": Get " + Describe(id) + ": " + st.ToString());
      return s->Fail(std::string(where) + ": Get " + Describe(id) + " returned '" + got +
                     "', expected NotFound");
    }
    if (!st.ok()) return s->Fail(std::string(where) + ": Get " + Describe(id) + ": " + st.ToString());
    if (got != it->second) {
      return s->Fail(std::string(where) + ": Get " + Describe(id) + " returned '" + got +
                     "', expected '" + it->second + "'");
    }
  }
  return true;
}

bool CheckAll(Scenario* s, ConfRng* rng, const char* where) {
  leveldb::ReadOptions ro;
  ro.verify_checksums = true;
  if (!CheckGets(s, ro, s->model, rng, 64, where)) return false;
  return CheckIteration(s, ro, s->model, where);
}

bool OpenDb(Scenario* s, bool create) {
  s->options = leveldb::Options();
  s->options.create_if_missing = create;
  s->options.merge_operator = &s->merge_op;
  s->options.paranoid_checks = true;
  s->options.write_buffer_size = 96 * 1024;
  s->options.max_file_size = 128 * 1024;
  s->options.block_size = 2048;
  s->options.compression = leveldb::kNoCompression;
  s->options.env = &s->env;
  leveldb::Status st = leveldb::DB::Open(s->options, s->dir, &s->db);
  s->env.WaitIdle();
  if (!st.ok()) return s->Fail("open: " + st.ToString());
  return true;
}

void CloseDb(Scenario* s) {
  if (s->db != nullptr) s->env.WaitIdle();
  delete s->db;
  s->db = nullptr;
  s->env.WaitIdle();
}

}  // namespace

namespace {

// Bytes this process has passed to write(2) and read(2) so far.
void SelfIo(uint64_t* wchar, uint64_t* rchar) {
  *wchar = *rchar = 0;
  FILE* f = fopen("/proc/self/io", "r");
  if (f == nullptr) return;
  char line[256];
  while (fgets(line, sizeof(line), f) != nullptr) {
    unsigned long long v = 0;
    if (sscanf(line, "wchar: %llu", &v) == 1) *wchar = v;
    else if (sscanf(line, "rchar: %llu", &v) == 1) *rchar = v;
  }
  fclose(f);
}

}  // namespace

void DestroyScenarioDir(const std::string& dir) {
  leveldb::Options o;
  leveldb::DestroyDB(dir, o);
  rmdir(dir.c_str());
}

bool RunPerfCase(const std::string& dir, const std::string& which, uint64_t scale,
                 std::string* error) {
  Scenario s;
  s.dir = dir;
  ConfRng rng(0x9E3779B9ull);
  DestroyScenarioDir(dir);
  mkdir(dir.c_str(), 0755);
  if (!OpenDb(&s, true)) {
    *error = s.error;
    return false;
  }
  const uint64_t keys = 40000 * scale;
  // Load, then flush everything to disk so the measured phase starts from a
  // quiet database.
  for (uint64_t id = 0; id < keys; id++) {
    std::string v = ConfValue(static_cast<long long>(id), 80);
    leveldb::Status st = s.db->Put(leveldb::WriteOptions(), Describe(id), v);
    if (!st.ok()) { *error = "load: " + st.ToString(); CloseDb(&s); return false; }
  }
  s.db->CompactRange(nullptr, nullptr);

  // Everything above is setup.  The kernel's counters for this process are
  // read either side of the phase below and printed; measure.py bounds the
  // whole process from outside as well.
  uint64_t w0 = 0, r0 = 0;
  SelfIo(&w0, &r0);
  if (which == "rangedel") {
    const uint64_t span = keys / 200;
    for (uint64_t i = 0; i < 200; i++) {
      uint64_t lo = (i * span) % keys;
      leveldb::Status st = s.db->DeleteRange(leveldb::WriteOptions(), Describe(lo),
                                             Describe(lo + span));
      if (!st.ok()) { *error = "DeleteRange: " + st.ToString(); CloseDb(&s); return false; }
    }
  } else if (which == "merge") {
    // spread over every loaded key, so folding an operand into its value at
    // write time means reading a block that is usually not cached
    for (uint64_t i = 0; i < 50000 * scale; i++) {
      uint64_t id = rng.Below(keys);
      leveldb::Status st = s.db->Merge(leveldb::WriteOptions(), Describe(id), "1");
      if (!st.ok()) { *error = "Merge: " + st.ToString(); CloseDb(&s); return false; }
    }
  } else {
    *error = "unknown case " + which;
    CloseDb(&s);
    return false;
  }
  uint64_t w1 = 0, r1 = 0;
  SelfIo(&w1, &r1);
  printf("PHASE_WCHAR %llu\nPHASE_RCHAR %llu\n",
         static_cast<unsigned long long>(w1 - w0), static_cast<unsigned long long>(r1 - r0));
  fflush(stdout);
  CloseDb(&s);
  return true;
}

bool RunScenario(const std::string& dir, uint64_t seed, uint64_t ops, bool verbose,
                 std::string* error) {
  Scenario s;
  s.dir = dir;
  ConfRng rng(seed ^ 0x5A5Aull);
  s.keys = 200 + rng.Below(1800);
  DestroyScenarioDir(dir);
  mkdir(dir.c_str(), 0755);
  if (!OpenDb(&s, true)) {
    *error = s.error;
    return false;
  }

  // A snapshot and the model as it was when the snapshot was taken.
  const leveldb::Snapshot* snap = nullptr;
  ConfModel snap_model;

  bool ok = true;
  for (uint64_t i = 0; i < ops && ok; i++) {
    uint64_t roll = rng.Below(100);
    if (roll < 30) {
      // a batch of puts, deletes and merges
      leveldb::WriteBatch batch;
      uint64_t n = 1 + rng.Below(8);
      for (uint64_t j = 0; j < n; j++) {
        uint64_t id = rng.Below(s.keys);
        uint64_t what = rng.Below(100);
        if (what < 55) {
          std::string v = ConfValue(s.next_int++, rng.Below(kMaxPad));
          batch.Put(Describe(id), v);
          ModelPut(&s.model, id, v);
        } else if (what < 75) {
          long long delta = 1 + static_cast<long long>(rng.Below(1000));
          batch.Merge(Describe(id), std::to_string(delta));
          ModelMerge(&s.model, id, delta);
        } else {
          batch.Delete(Describe(id));
          ModelDelete(&s.model, id);
        }
      }
      leveldb::Status st = s.db->Write(leveldb::WriteOptions(), &batch);
      if (!st.ok()) { ok = s.Fail("Write: " + st.ToString()); break; }
    } else if (roll < 45) {
      std::string v = ConfValue(s.next_int++, rng.Below(kMaxPad));
      uint64_t id = rng.Below(s.keys);
      leveldb::Status st = s.db->Put(leveldb::WriteOptions(), Describe(id), v);
      if (!st.ok()) { ok = s.Fail("Put: " + st.ToString()); break; }
      ModelPut(&s.model, id, v);
    } else if (roll < 55) {
      long long delta = 1 + static_cast<long long>(rng.Below(1000));
      uint64_t id = rng.Below(s.keys);
      leveldb::Status st = s.db->Merge(leveldb::WriteOptions(), Describe(id), std::to_string(delta));
      if (!st.ok()) { ok = s.Fail("Merge: " + st.ToString()); break; }
      ModelMerge(&s.model, id, delta);
    } else if (roll < 70) {
      // range delete, sometimes wide, sometimes a handful of keys
      uint64_t lo = rng.Below(s.keys);
      uint64_t span = rng.Chance(25) ? 1 + rng.Below(s.keys) : 1 + rng.Below(s.keys / 8 + 2);
      uint64_t hi = lo + span;
      uint64_t shape = rng.Below(100);
      if (shape < 40) {
        leveldb::Status st = s.db->DeleteRange(leveldb::WriteOptions(), Describe(lo), Describe(hi));
        if (!st.ok()) { ok = s.Fail("DeleteRange: " + st.ToString()); break; }
        ModelDeleteRange(&s.model, lo, hi);
      } else {
        // the same batch writes around the tombstone: what comes before it
        // must be swept away, what comes after it must survive
        leveldb::WriteBatch batch;
        uint64_t before = lo + rng.Below(span);
        std::string bv = ConfValue(s.next_int++, rng.Below(kMaxPad));
        batch.Put(Describe(before), bv);
        ModelPut(&s.model, before, bv);
        if (shape >= 70) {
          long long delta = 1 + static_cast<long long>(rng.Below(1000));
          uint64_t mid = lo + rng.Below(span);
          batch.Merge(Describe(mid), std::to_string(delta));
          ModelMerge(&s.model, mid, delta);
        }
        batch.DeleteRange(Describe(lo), Describe(hi));
        ModelDeleteRange(&s.model, lo, hi);
        if (shape >= 55) {
          uint64_t after = lo + rng.Below(span);
          std::string av = ConfValue(s.next_int++, rng.Below(kMaxPad));
          batch.Put(Describe(after), av);
          ModelPut(&s.model, after, av);
        }
        if (shape >= 85) {
          // a second, empty or reversed range: a no-op
          batch.DeleteRange(Describe(hi), Describe(lo));
        }
        leveldb::Status st = s.db->Write(leveldb::WriteOptions(), &batch);
        if (!st.ok()) { ok = s.Fail("Write(DeleteRange): " + st.ToString()); break; }
      }
    } else if (roll < 80) {
      leveldb::ReadOptions ro;
      ok = CheckGets(&s, ro, s.model, &rng, 16, "reads");
    } else if (roll < 86) {
      leveldb::ReadOptions ro;
      ok = CheckIteration(&s, ro, s.model, "iteration");
    } else if (roll < 92) {
      if (snap == nullptr) {
        snap = s.db->GetSnapshot();
        snap_model = s.model;
      } else {
        leveldb::ReadOptions ro;
        ro.snapshot = snap;
        ok = CheckGets(&s, ro, snap_model, &rng, 24, "snapshot reads") &&
             CheckIteration(&s, ro, snap_model, "snapshot iteration");
        s.db->ReleaseSnapshot(snap);
        snap = nullptr;
      }
    } else if (roll < 96) {
      if (snap != nullptr) {
        s.db->ReleaseSnapshot(snap);
        snap = nullptr;
      }
      CloseDb(&s);
      if (!OpenDb(&s, false)) { ok = false; break; }
      ok = CheckAll(&s, &rng, "after reopen");
    } else {
      s.db->CompactRange(nullptr, nullptr);
      ok = CheckAll(&s, &rng, "after compaction");
    }
    if (verbose && (i % 50) == 0) {
      fprintf(stderr, "op %llu: %zu live keys\n", static_cast<unsigned long long>(i),
              s.model.size());
    }
  }

  if (ok) {
    if (snap != nullptr) {
      leveldb::ReadOptions ro;
      ro.snapshot = snap;
      ok = CheckGets(&s, ro, snap_model, &rng, 24, "final snapshot reads") &&
           CheckIteration(&s, ro, snap_model, "final snapshot iteration");
    }
  }
  if (snap != nullptr) s.db->ReleaseSnapshot(snap);
  if (ok) ok = CheckAll(&s, &rng, "final");
  if (ok) {
    CloseDb(&s);
    if (OpenDb(&s, false)) {
      ok = CheckAll(&s, &rng, "final reopen");
    } else {
      ok = false;
    }
  }
  CloseDb(&s);
  if (!ok) *error = s.error;
  return ok;
}

}  // namespace lsmbench
