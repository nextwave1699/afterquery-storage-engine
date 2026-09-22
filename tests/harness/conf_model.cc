#include "conf_model.h"

#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <memory>

#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "leveldb/iterator.h"
#include "leveldb/options.h"
#include "leveldb/write_batch.h"

#include "bench_env.h"

namespace lsmbench {

// ---- the merge operator ---------------------------------------------------

bool AffineParseOperand(const char* p, size_t n, uint64_t* a, uint64_t* b) {
  std::string s(p, n);
  size_t comma = s.find(',');
  if (comma == std::string::npos || comma == 0 || comma + 1 >= s.size()) return false;
  char* end = nullptr;
  *a = strtoull(s.c_str(), &end, 10);
  if (end != s.c_str() + comma) return false;
  *b = strtoull(s.c_str() + comma + 1, &end, 10);
  if (end != s.c_str() + s.size()) return false;
  return *a < kAffineP && *b < kAffineP;
}

uint64_t AffineValueX(const std::string& key, const char* existing, size_t n, bool has_existing) {
  if (!has_existing) return AffineHash(key.data(), key.size());
  if (n > 1 && existing[0] == 'M') {
    return strtoull(std::string(existing + 1, n - 1).c_str(), nullptr, 10) % kAffineP;
  }
  return AffineHash(existing, n);
}

std::string AffineMerged(uint64_t x, size_t length) {
  // fixed width, so that folding operands one at a time or all at once
  // gives the same length
  char buf[32];
  snprintf(buf, sizeof(buf), "M%019llu:", static_cast<unsigned long long>(x));
  std::string v(buf);
  if (v.size() < length) v.append(length - v.size(), 'm');
  return v;
}

std::string AffineFullMerge(const std::string& key, const std::string* existing,
                            const std::vector<std::string>& operands) {
  uint64_t x = existing == nullptr ? AffineValueX(key, nullptr, 0, false)
                                   : AffineValueX(key, existing->data(), existing->size(), true);
  for (const std::string& op : operands) {
    uint64_t a = 0, b = 0;
    if (!AffineParseOperand(op.data(), op.size(), &a, &b)) return "BAD-OPERAND";
    x = AffineAdd(AffineMul(a, x), b);
  }
  return AffineMerged(x, existing == nullptr ? 0 : existing->size());
}

#ifndef CONF_PRISTINE
bool AffineOperator::FullMerge(const leveldb::Slice& key, const leveldb::Slice* existing,
                               const std::vector<std::string>& operands,
                               std::string* new_value) const {
  for (const std::string& op : operands) {
    uint64_t a, b;
    if (!AffineParseOperand(op.data(), op.size(), &a, &b)) return false;
  }
  std::string k = key.ToString();
  if (existing == nullptr) {
    *new_value = AffineFullMerge(k, nullptr, operands);
  } else {
    std::string e = existing->ToString();
    *new_value = AffineFullMerge(k, &e, operands);
  }
  return true;
}

bool AffineOperator::PartialMerge(const leveldb::Slice& key, const leveldb::Slice& left,
                                  const leveldb::Slice& right, std::string* result) const {
  (void)key;
  uint64_t a1, b1, a2, b2;
  if (!AffineParseOperand(left.data(), left.size(), &a1, &b1) ||
      !AffineParseOperand(right.data(), right.size(), &a2, &b2)) {
    return false;
  }
  // left first, then right: x -> a2*(a1*x + b1) + b2
  *result = AffineOperand(AffineMul(a2, a1), AffineAdd(AffineMul(a2, b1), b2));
  return true;
}
#endif

// ---- scenario generation ----------------------------------------------------

ConfShape ShapeFor(uint64_t seed) {
  ConfRng r(seed ^ 0x5AA5ull);
  ConfShape s;
  s.keys = 300 + r.Below(2700);
  s.bloom = r.Chance(50);
  s.reuse_logs = r.Chance(20);
  s.small_cache = r.Chance(30);
  // Small buffers and files so a few hundred operations already produce
  // flushes, several levels and compactions.
  s.write_buffer = static_cast<uint32_t>(32 * 1024 + r.Below(64 * 1024));
  s.max_file = static_cast<uint32_t>(48 * 1024 + r.Below(96 * 1024));
  s.block_size = r.Chance(50) ? 512 : 2048;
  return s;
}

ScenarioGen::ScenarioGen(uint64_t seed, uint64_t ops, bool ext)
    : rng_(seed ^ 0xC0DEull), shape_(ShapeFor(seed)), ops_(ops), ext_(ext) {}

void ScenarioGen::RandomWrite(ConfWrite* w, uint64_t id, bool allow_merge) {
  w->id = id;
  w->end = 0;
  uint64_t roll = rng_.Below(100);
  if (allow_merge && roll < 30) {
    w->kind = kWMerge;
    w->value = AffineOperand(1 + rng_.Below(kAffineP - 1), rng_.Below(kAffineP));
  } else if (roll < 75) {
    w->kind = kWPut;
    w->value = ConfValue(id, version_++, rng_.Below(160));
  } else {
    w->kind = kWDelete;
    w->value.clear();
  }
}

bool ScenarioGen::Next(const ConfModel& model, ConfOp* op) {
  *op = ConfOp();
  if (done_ >= ops_) return false;
  done_++;
  const uint64_t keys = shape_.keys;
  uint64_t roll = rng_.Below(100);
  if (roll < 12) {
    // a single put or delete
    op->kind = kOpWrite;
    op->single = true;
    ConfWrite w;
    RandomWrite(&w, rng_.Below(keys), false);
    op->writes.push_back(w);
  } else if (roll < 30 && !(ext_ && roll >= 20)) {
    // a batch of puts and deletes (and merges)
    op->kind = kOpWrite;
    uint64_t n = 1 + rng_.Below(rng_.Chance(10) ? 200 : 12);
    for (uint64_t i = 0; i < n; i++) {
      ConfWrite w;
      RandomWrite(&w, rng_.Below(keys), ext_);
      op->writes.push_back(w);
    }
  } else if (roll < 25) {
    // a single merge
    op->kind = kOpWrite;
    op->single = true;
    ConfWrite w;
    w.kind = kWMerge;
    w.id = rng_.Chance(50) ? rng_.Below(keys) : rng_.Below(keys / 10 + 1);  // some hot keys
    w.end = 0;
    w.value = AffineOperand(1 + rng_.Below(kAffineP - 1), rng_.Below(kAffineP));
    op->writes.push_back(w);
  } else if (roll < 30) {
    // a single range delete, sometimes wide, sometimes a few keys, now and
    // then empty or reversed
    op->kind = kOpWrite;
    op->single = true;
    ConfWrite w;
    w.kind = kWDeleteRange;
    w.id = rng_.Below(keys + 10);
    uint64_t span = rng_.Chance(20) ? 1 + rng_.Below(keys) : 1 + rng_.Below(keys / 12 + 2);
    w.end = rng_.Chance(5) ? w.id - rng_.Below(w.id + 1) : w.id + span;
    op->writes.push_back(w);
  } else if (roll < 38) {
    // clear a key range in one batch, then write a few keys back into it;
    // with the extensions, sometimes with a range delete in the middle of
    // writes that it must and must not hide
    op->kind = kOpWrite;
    uint64_t lo = rng_.Below(keys);
    uint64_t hi = lo + 1 + rng_.Below(keys / 4 + 1);
    if (ext_ && rng_.Chance(60)) {
      uint64_t before = rng_.Below(4);
      for (uint64_t i = 0; i < before; i++) {
        ConfWrite w;
        RandomWrite(&w, lo + rng_.Below(hi - lo + 2), true);
        op->writes.push_back(w);
      }
      ConfWrite d;
      d.kind = kWDeleteRange;
      d.id = lo;
      d.end = hi;
      op->writes.push_back(d);
      uint64_t after = rng_.Below(4);
      for (uint64_t i = 0; i < after; i++) {
        ConfWrite w;
        RandomWrite(&w, lo + rng_.Below(hi - lo + 2), true);
        op->writes.push_back(w);
      }
      if (rng_.Chance(20)) {
        ConfWrite d2;
        d2.kind = kWDeleteRange;
        d2.id = lo + rng_.Below(hi - lo);
        d2.end = d2.id + rng_.Below(keys / 8 + 1);
        op->writes.push_back(d2);
      }
    } else {
      for (auto it = model.lower_bound(lo); it != model.end() && it->first < hi; ++it) {
        op->writes.push_back({kWDelete, it->first, 0, ""});
      }
      uint64_t back = rng_.Below(4);
      for (uint64_t i = 0; i < back; i++) {
        uint64_t id = lo + rng_.Below(hi - lo);
        op->writes.push_back({kWPut, id, 0, ConfValue(id, version_++, rng_.Below(160))});
      }
      if (op->writes.empty()) op->writes.push_back({kWDelete, lo, 0, ""});
    }
  } else if (roll < 50) {
    op->kind = kOpGets;
    op->count = 8 + rng_.Below(24);
  } else if (roll < 64) {
    op->kind = kOpWalk;
    op->id = rng_.Below(keys + 20);
    op->count = 1 + rng_.Below(80);
  } else if (roll < 70) {
    op->kind = kOpScan;
  } else if (roll < 84) {
    if (snaps_open_ > 0 && (snaps_open_ >= 3 || rng_.Chance(45))) {
      op->kind = kOpSnapCheck;
      op->id = rng_.Below(snaps_open_);
      snaps_open_--;
    } else {
      op->kind = kOpSnapTake;
      snaps_open_++;
    }
  } else if (roll < 88) {
    op->kind = kOpReopen;
    snaps_open_ = 0;
  } else if (roll < 94) {
    op->kind = kOpCompact;
    if (rng_.Chance(35)) {
      op->count = 0;
    } else {
      op->id = rng_.Below(keys);
      op->count = 1 + rng_.Below(keys / 3 + 1);
    }
  } else {
    // a burst of small writes to push data through flushes
    op->kind = kOpWrite;
    uint64_t n = 50 + rng_.Below(250);
    uint64_t base = rng_.Below(keys);
    for (uint64_t i = 0; i < n; i++) {
      ConfWrite w;
      RandomWrite(&w, (base + i * 7) % keys, ext_);
      op->writes.push_back(w);
    }
  }
  return true;
}

void ScenarioGen::Apply(const ConfOp& op, ConfModel* model) {
  if (op.kind != kOpWrite) return;
  for (const ConfWrite& w : op.writes) {
    switch (w.kind) {
      case kWPut:
        (*model)[w.id] = w.value;
        break;
      case kWDelete:
        model->erase(w.id);
        break;
      case kWMerge: {
        auto it = model->find(w.id);
        std::vector<std::string> ops{w.value};
        std::string v = AffineFullMerge(ConfKey(w.id), it == model->end() ? nullptr : &it->second, ops);
        (*model)[w.id] = v;
        break;
      }
      case kWDeleteRange:
        if (w.id < w.end) model->erase(model->lower_bound(w.id), model->lower_bound(w.end));
        break;
    }
  }
}

ConfModel ModelAfterWrites(uint64_t seed, uint64_t ops, bool ext, uint64_t writes, bool* past_end) {
  ScenarioGen gen(seed, ops, ext);
  ConfModel model;
  ConfOp op;
  uint64_t n = 0;
  *past_end = false;
  while (n < writes) {
    if (!gen.Next(model, &op)) {
      *past_end = true;
      break;
    }
    if (op.kind != kOpWrite) continue;
    ScenarioGen::Apply(op, &model);
    n++;
  }
  return model;
}

void DestroyScenarioDir(const std::string& dir) {
  // Remove every file, not only the ones LevelDB knows about.
  DIR* d = opendir(dir.c_str());
  if (d != nullptr) {
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      unlink((dir + "/" + e->d_name).c_str());
    }
    closedir(d);
  }
  rmdir(dir.c_str());
}

// ---- running scenarios ------------------------------------------------------

namespace {

struct Db {
  BenchEnv env;
  leveldb::DB* db = nullptr;
  std::string dir;
  std::string error;
  ConfShape shape{};
  bool ext = false;
  std::unique_ptr<const leveldb::FilterPolicy> bloom;
  std::unique_ptr<leveldb::Cache> cache;
#ifndef CONF_PRISTINE
  AffineOperator merge_op;
#endif

  bool Fail(const std::string& what) {
    if (error.empty()) error = what;
    return false;
  }

  bool Open(bool create) {
    leveldb::Options o;
    o.create_if_missing = create;
    o.paranoid_checks = true;
    o.env = &env;
    o.write_buffer_size = shape.write_buffer;
    o.max_file_size = shape.max_file;
    o.block_size = shape.block_size;
    o.compression = leveldb::kNoCompression;
    o.reuse_logs = shape.reuse_logs;
    if (shape.bloom) {
      if (!bloom) bloom.reset(leveldb::NewBloomFilterPolicy(10));
      o.filter_policy = bloom.get();
    }
    if (shape.small_cache) {
      if (!cache) cache.reset(leveldb::NewLRUCache(16 * 1024));
      o.block_cache = cache.get();
    }
#ifndef CONF_PRISTINE
    if (ext) o.merge_operator = &merge_op;
#endif
    leveldb::Status st = leveldb::DB::Open(o, dir, &db);
    env.WaitIdle();
    if (!st.ok()) return Fail("open: " + st.ToString());
    return true;
  }

  void Close() {
    if (db != nullptr) env.WaitIdle();
    delete db;
    db = nullptr;
    env.WaitIdle();
  }
};

std::string KeyName(uint64_t id) { return ConfKey(id); }

bool CheckGets(Db* d, const leveldb::ReadOptions& ro, const ConfModel& model, ConfRng* rng,
               uint64_t keys, uint64_t n, const char* where) {
  for (uint64_t i = 0; i < n; i++) {
    uint64_t id = rng->Below(keys + 20);
    std::string got;
    leveldb::Status st = d->db->Get(ro, KeyName(id), &got);
    d->env.WaitIdle();
    auto it = model.find(id);
    if (it == model.end()) {
      if (st.IsNotFound()) continue;
      if (!st.ok()) return d->Fail(std::string(where) + ": Get " + KeyName(id) + ": " + st.ToString());
      return d->Fail(std::string(where) + ": Get " + KeyName(id) + " found '" + got +
                     "', expected NotFound");
    }
    if (!st.ok()) return d->Fail(std::string(where) + ": Get " + KeyName(id) + ": " + st.ToString() +
                                 ", expected '" + it->second + "'");
    if (got != it->second) {
      return d->Fail(std::string(where) + ": Get " + KeyName(id) + " returned '" + got +
                     "', expected '" + it->second + "'");
    }
  }
  return true;
}

bool SameEntry(Db* d, leveldb::Iterator* it, ConfModel::const_iterator e, const char* where) {
  if (it->key().ToString() != KeyName(e->first)) {
    return d->Fail(std::string(where) + ": iterator at " + it->key().ToString() + ", expected " +
                   KeyName(e->first));
  }
  if (it->value().ToString() != e->second) {
    return d->Fail(std::string(where) + ": value at " + it->key().ToString() + " is '" +
                   it->value().ToString() + "', expected '" + e->second + "'");
  }
  return true;
}

bool CheckScan(Db* d, const leveldb::ReadOptions& ro, const ConfModel& model, const char* where) {
  std::unique_ptr<leveldb::Iterator> it(d->db->NewIterator(ro));
  auto e = model.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++e) {
    if (e == model.end()) return d->Fail(std::string(where) + ": extra key " + it->key().ToString());
    if (!SameEntry(d, it.get(), e, where)) return false;
  }
  if (!it->status().ok()) return d->Fail(std::string(where) + ": " + it->status().ToString());
  if (e != model.end()) return d->Fail(std::string(where) + ": scan stopped before " + KeyName(e->first));
  auto r = model.rbegin();
  for (it->SeekToLast(); it->Valid(); it->Prev(), ++r) {
    if (r == model.rend()) return d->Fail(std::string(where) + ": extra key backwards " + it->key().ToString());
    if (it->key().ToString() != KeyName(r->first) || it->value().ToString() != r->second) {
      return d->Fail(std::string(where) + ": backwards at " + it->key().ToString() + " = '" +
                     it->value().ToString() + "', expected " + KeyName(r->first) + " = '" + r->second + "'");
    }
  }
  if (!it->status().ok()) return d->Fail(std::string(where) + ": " + it->status().ToString());
  if (r != model.rend()) return d->Fail(std::string(where) + ": backwards scan stopped before " + KeyName(r->first));
  it.reset();
  d->env.WaitIdle();
  return true;
}

// Seek, then wander with Next and Prev, changing direction at random: the
// part of DBIter that is easiest to get wrong.
bool CheckWalk(Db* d, const leveldb::ReadOptions& ro, const ConfModel& model, uint64_t start,
               uint64_t steps, ConfRng* rng) {
  std::unique_ptr<leveldb::Iterator> it(d->db->NewIterator(ro));
  it->Seek(KeyName(start));
  auto e = model.lower_bound(start);
  for (uint64_t i = 0;; i++) {
    bool valid = e != model.end();
    if (it->Valid() != valid) {
      return d->Fail("walk from " + KeyName(start) + " step " + std::to_string(i) + ": iterator " +
                     (it->Valid() ? "at " + it->key().ToString() : std::string("invalid")) +
                     ", expected " + (valid ? KeyName(e->first) : std::string("the end")) +
                     (it->status().ok() ? "" : " (" + it->status().ToString() + ")"));
    }
    if (!valid) break;
    if (!SameEntry(d, it.get(), e, "walk")) return false;
    if (i == steps) break;
    if (rng->Chance(60)) {
      it->Next();
      ++e;
    } else {
      if (e == model.begin()) {
        it->Prev();
        if (it->Valid()) return d->Fail("walk: Prev past the first key returned " + it->key().ToString());
        break;
      }
      it->Prev();
      --e;
    }
  }
  if (!it->status().ok()) return d->Fail("walk: " + it->status().ToString());
  it.reset();
  d->env.WaitIdle();
  return true;
}

void WriteAck(int fd, uint64_t n) {
  if (fd < 0) return;
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%llu\n", static_cast<unsigned long long>(n));
  if (pwrite(fd, buf, len, 0) != len || ftruncate(fd, len) != 0) {
    fprintf(stderr, "conftest: ack write failed\n");
    _exit(4);
  }
}

leveldb::Status DoWrite(Db* d, const ConfOp& op) {
  leveldb::WriteOptions wo;
  if (op.single) {
    const ConfWrite& w = op.writes[0];
    switch (w.kind) {
      case kWPut:
        return d->db->Put(wo, KeyName(w.id), w.value);
      case kWDelete:
        return d->db->Delete(wo, KeyName(w.id));
#ifndef CONF_PRISTINE
      case kWMerge:
        return d->db->Merge(wo, KeyName(w.id), w.value);
      case kWDeleteRange:
        return d->db->DeleteRange(wo, KeyName(w.id), KeyName(w.end));
#endif
      default:
        return leveldb::Status::NotSupported("extension write in a stock scenario");
    }
  }
  leveldb::WriteBatch batch;
  for (const ConfWrite& w : op.writes) {
    switch (w.kind) {
      case kWPut:
        batch.Put(KeyName(w.id), w.value);
        break;
      case kWDelete:
        batch.Delete(KeyName(w.id));
        break;
#ifndef CONF_PRISTINE
      case kWMerge:
        batch.Merge(KeyName(w.id), w.value);
        break;
      case kWDeleteRange:
        batch.DeleteRange(KeyName(w.id), KeyName(w.end));
        break;
#endif
      default:
        return leveldb::Status::NotSupported("extension write in a stock scenario");
    }
  }
  return d->db->Write(wo, &batch);
}

std::string DescribeWrite(const ConfOp& op) {
  std::string s = op.single ? "single " : "batch of " + std::to_string(op.writes.size()) + " ";
  const ConfWrite& w = op.writes[0];
  const char* names[] = {"Put", "Delete", "Merge", "DeleteRange"};
  s += names[w.kind];
  s += " " + KeyName(w.id);
  if (w.kind == kWDeleteRange) s += ".." + KeyName(w.end);
  return s;
}

}  // namespace

bool RunScenario(const std::string& dir, uint64_t seed, uint64_t ops, bool ext,
                 const std::string& crash, int ack_fd, std::string* error) {
  Db d;
  d.dir = dir;
  d.ext = ext;
  d.shape = ShapeFor(seed);
  if (!d.env.crash()->Configure(crash)) {
    *error = "bad crash spec " + crash;
    return false;
  }
  DestroyScenarioDir(dir);
  mkdir(dir.c_str(), 0755);
  // acknowledge nothing yet: the crash may come while the database is
  // still being created
  WriteAck(ack_fd, 0);
  if (!d.Open(true)) {
    *error = d.error;
    return false;
  }
  ScenarioGen gen(seed, ops, ext);
  ConfRng check_rng(seed ^ 0x7E57ull);
  ConfModel model;
  struct Snap {
    const leveldb::Snapshot* snap;
    ConfModel model;
  };
  std::vector<Snap> snaps;
  uint64_t writes = 0, opno = 0;
  ConfOp op;
  bool ok = true;
  while (ok && gen.Next(model, &op)) {
    opno++;
    leveldb::ReadOptions ro;
    std::string where = "op " + std::to_string(opno);
    switch (op.kind) {
      case kOpWrite: {
        leveldb::Status st = DoWrite(&d, op);
        d.env.WaitIdle();
        if (!st.ok()) { ok = d.Fail(where + ": write (" + DescribeWrite(op) + "): " + st.ToString()); break; }
        ScenarioGen::Apply(op, &model);
        WriteAck(ack_fd, ++writes);
        break;
      }
      case kOpGets:
        ok = CheckGets(&d, ro, model, &check_rng, gen.keys(), op.count, (where + " reads").c_str());
        break;
      case kOpWalk:
        ok = CheckWalk(&d, ro, model, op.id, op.count, &check_rng);
        break;
      case kOpScan:
        ok = CheckScan(&d, ro, model, (where + " scan").c_str());
        break;
      case kOpSnapTake:
        snaps.push_back({d.db->GetSnapshot(), model});
        break;
      case kOpSnapCheck:
        if (op.id < snaps.size()) {
          ro.snapshot = snaps[op.id].snap;
          ok = CheckGets(&d, ro, snaps[op.id].model, &check_rng, gen.keys(), 24,
                         (where + " snapshot reads").c_str()) &&
               CheckScan(&d, ro, snaps[op.id].model, (where + " snapshot scan").c_str()) &&
               CheckWalk(&d, ro, snaps[op.id].model, check_rng.Below(gen.keys()), 30, &check_rng);
          d.db->ReleaseSnapshot(snaps[op.id].snap);
          snaps.erase(snaps.begin() + op.id);
        }
        break;
      case kOpReopen:
        for (Snap& s : snaps) d.db->ReleaseSnapshot(s.snap);
        snaps.clear();
        d.Close();
        ok = d.Open(false) && CheckGets(&d, ro, model, &check_rng, gen.keys(), 32, (where + " after reopen").c_str());
        break;
      case kOpCompact:
        if (op.count == 0) {
          d.db->CompactRange(nullptr, nullptr);
        } else {
          std::string lo = KeyName(op.id), hi = KeyName(op.id + op.count);
          leveldb::Slice b(lo), e(hi);
          d.db->CompactRange(&b, &e);
        }
        d.env.WaitIdle();
        ok = CheckGets(&d, ro, model, &check_rng, gen.keys(), 32, (where + " after compaction").c_str());
        if (ok && !snaps.empty()) {
          // what the compaction kept for the oldest snapshot
          leveldb::ReadOptions sro;
          sro.snapshot = snaps[0].snap;
          ok = CheckScan(&d, sro, snaps[0].model, (where + " snapshot scan after compaction").c_str());
        }
        break;
    }
  }
  if (ok) {
    for (size_t i = 0; ok && i < snaps.size(); i++) {
      leveldb::ReadOptions ro;
      ro.snapshot = snaps[i].snap;
      ok = CheckScan(&d, ro, snaps[i].model, "final snapshot scan");
    }
  }
  for (Snap& s : snaps) d.db->ReleaseSnapshot(s.snap);
  if (ok) ok = CheckScan(&d, leveldb::ReadOptions(), model, "final scan");
  if (ok) {
    d.Close();
    ok = d.Open(false) && CheckScan(&d, leveldb::ReadOptions(), model, "final reopen");
  }
  if (ok) {
    // compacted all the way down, nothing may change
    d.db->CompactRange(nullptr, nullptr);
    d.env.WaitIdle();
    ok = CheckScan(&d, leveldb::ReadOptions(), model, "after final compaction");
  }
  d.Close();
  if (!ok) *error = d.error;
  return ok;
}

bool CheckRecovered(const std::string& dir, uint64_t seed, uint64_t ops, bool ext,
                    uint64_t acked, uint64_t* recovered, std::string* error) {
  bool end0 = false, end1 = false;
  ConfModel m0 = ModelAfterWrites(seed, ops, ext, acked, &end0);
  ConfModel m1 = ModelAfterWrites(seed, ops, ext, acked + 1, &end1);
  Db d;
  d.dir = dir;
  d.ext = ext;
  d.shape = ShapeFor(seed);
  // With nothing acknowledged the crash may have come before the database
  // existed, so it may be created here.
  if (!d.Open(acked == 0)) {
    *error = d.error;
    return false;
  }
  // Which state is it?  Try the acknowledged one first; a database that
  // matches neither lost acknowledged writes or kept half a batch.
  bool is0 = CheckScan(&d, leveldb::ReadOptions(), m0, "recovered");
  std::string err0 = d.error;
  d.error.clear();
  bool is1 = !is0 && !end1 && CheckScan(&d, leveldb::ReadOptions(), m1, "recovered");
  if (!is0 && !is1) {
    *error = "recovered state matches neither " + std::to_string(acked) + " nor " +
             std::to_string(acked + 1) + " writes: " + err0;
    d.Close();
    return false;
  }
  *recovered = is0 ? acked : acked + 1;
  // The database has to be usable afterwards: write, reopen, read back.
  ConfModel& m = is0 ? m0 : m1;
  ConfRng rng(seed ^ 0xAF7Eull);
  for (uint64_t i = 0; i < 60; i++) {
    ConfOp op;
    op.kind = kOpWrite;
    op.single = true;
    uint64_t id = rng.Below(d.shape.keys);
    ConfWrite w{kWPut, id, 0, ConfValue(id, 1000000000ull + i, 40)};
    if (ext && i % 3 == 1) {
      w = ConfWrite{kWMerge, id, 0, AffineOperand(1 + rng.Below(kAffineP - 1), rng.Below(kAffineP))};
    } else if (ext && i % 10 == 9) {
      w = ConfWrite{kWDeleteRange, id, id + 1 + rng.Below(30), ""};
    }
    op.writes.push_back(w);
    leveldb::Status st = DoWrite(&d, op);
    d.env.WaitIdle();
    if (!st.ok()) {
      *error = "write after recovery: " + st.ToString();
      d.Close();
      return false;
    }
    ScenarioGen::Apply(op, &m);
  }
  d.Close();
  bool ok = d.Open(false) && CheckScan(&d, leveldb::ReadOptions(), m, "after recovery and reopen");
  d.Close();
  if (!ok) *error = d.error;
  return ok;
}

#ifndef CONF_PRISTINE

// ---- API checks ---------------------------------------------------------------

namespace {

class Recorder : public leveldb::WriteBatch::Handler {
 public:
  std::vector<std::string> seen;
  void Put(const leveldb::Slice& k, const leveldb::Slice& v) override {
    seen.push_back("put " + k.ToString() + "=" + v.ToString());
  }
  void Delete(const leveldb::Slice& k) override { seen.push_back("del " + k.ToString()); }
  void Merge(const leveldb::Slice& k, const leveldb::Slice& v) override {
    seen.push_back("merge " + k.ToString() + "=" + v.ToString());
  }
  void DeleteRange(const leveldb::Slice& b, const leveldb::Slice& e) override {
    seen.push_back("delrange " + b.ToString() + ".." + e.ToString());
  }
};

// Only overrides the two stock methods: the new ones must have defaults.
class OldRecorder : public leveldb::WriteBatch::Handler {
 public:
  std::vector<std::string> seen;
  void Put(const leveldb::Slice& k, const leveldb::Slice& v) override {
    seen.push_back("put " + k.ToString());
  }
  void Delete(const leveldb::Slice& k) override { seen.push_back("del " + k.ToString()); }
};

std::string Join(const std::vector<std::string>& v) {
  std::string s;
  for (const std::string& x : v) s += (s.empty() ? "" : "; ") + x;
  return s;
}

}  // namespace

bool RunApiChecks(const std::string& dir, int* passed, int* total) {
  *passed = *total = 0;
  auto check = [&](const std::string& name, bool ok, const std::string& detail) {
    (*total)++;
    if (ok) (*passed)++;
    printf("API %s %s%s%s\n", name.c_str(), ok ? "ok" : "FAIL", ok ? "" : " ", ok ? "" : detail.c_str());
    fflush(stdout);
  };

  // 1. WriteBatch records, in order, through Iterate and Append.
  {
    leveldb::WriteBatch b;
    b.Put("a", "1");
    b.Merge("b", "2,3");
    b.DeleteRange("c", "f");
    b.Delete("d");
    Recorder r;
    leveldb::Status st = b.Iterate(&r);
    std::string want = "put a=1; merge b=2,3; delrange c..f; del d";
    check("batch-iterate", st.ok() && Join(r.seen) == want, st.ToString() + " got [" + Join(r.seen) + "]");
    leveldb::WriteBatch c;
    c.DeleteRange("x", "z");
    c.Append(b);
    c.Merge("q", "5,6");
    Recorder r2;
    st = c.Iterate(&r2);
    std::string want2 = "delrange x..z; " + want + "; merge q=5,6";
    check("batch-append", st.ok() && Join(r2.seen) == want2, st.ToString() + " got [" + Join(r2.seen) + "]");
    OldRecorder r3;
    st = b.Iterate(&r3);
    check("batch-default-handler", st.ok() && Join(r3.seen) == "put a; del d",
          st.ToString() + " got [" + Join(r3.seen) + "]");
    leveldb::WriteBatch copy = b;
    Recorder r4;
    st = copy.Iterate(&r4);
    check("batch-copy", st.ok() && Join(r4.seen) == want, st.ToString());
    check("batch-size", c.ApproximateSize() > b.ApproximateSize(), "ApproximateSize did not grow");
  }

  AffineOperator op;
  BenchEnv env;
  auto open = [&](const std::string& path, bool with_op, leveldb::DB** db) {
    leveldb::Options o;
    o.create_if_missing = true;
    o.env = &env;
    o.write_buffer_size = 64 * 1024;
    o.compression = leveldb::kNoCompression;
    if (with_op) o.merge_operator = &op;
    leveldb::Status st = leveldb::DB::Open(o, path, db);
    env.WaitIdle();
    return st;
  };
  auto get = [&](leveldb::DB* db, const std::string& k) {
    std::string v;
    leveldb::Status st = db->Get(leveldb::ReadOptions(), k, &v);
    env.WaitIdle();
    return st.ok() ? v : (st.IsNotFound() ? std::string("<none>") : "<" + st.ToString() + ">");
  };

  // 2. Without a merge operator, merges are refused and a batch holding one
  //    is refused whole.
  {
    std::string path = dir + "/no-operator";
    DestroyScenarioDir(path);
    mkdir(path.c_str(), 0755);
    leveldb::DB* db = nullptr;
    leveldb::Status st = open(path, false, &db);
    if (!st.ok()) {
      check("open", false, st.ToString());
      return false;
    }
    st = db->Merge(leveldb::WriteOptions(), "k", "1,1");
    env.WaitIdle();
    check("merge-without-operator", st.IsInvalidArgument(), "got " + st.ToString());
    leveldb::WriteBatch b;
    b.Put("p", "v");
    b.Merge("k", "1,1");
    st = db->Write(leveldb::WriteOptions(), &b);
    env.WaitIdle();
    check("batch-merge-without-operator", st.IsInvalidArgument() && get(db, "p") == "<none>",
          "got " + st.ToString() + ", p=" + get(db, "p"));
    st = db->DeleteRange(leveldb::WriteOptions(), "a", "z");
    env.WaitIdle();
    check("delete-range-without-operator", st.ok(), st.ToString());
    delete db;
    env.WaitIdle();
    DestroyScenarioDir(path);
  }

  // 3. Empty and reversed ranges delete nothing; the begin key is included
  //    and the end key excluded; a put after the tombstone in the same batch
  //    survives, one before it does not.
  {
    std::string path = dir + "/ranges";
    DestroyScenarioDir(path);
    mkdir(path.c_str(), 0755);
    leveldb::DB* db = nullptr;
    leveldb::Status st = open(path, true, &db);
    if (!st.ok()) {
      check("open", false, st.ToString());
      return false;
    }
    leveldb::WriteOptions wo;
    for (const char* k : {"b", "c", "d", "e", "f"}) db->Put(wo, k, std::string("v") + k);
    env.WaitIdle();
    db->DeleteRange(wo, "d", "d");
    db->DeleteRange(wo, "e", "c");
    env.WaitIdle();
    check("empty-and-reversed-ranges", get(db, "c") == "vc" && get(db, "d") == "vd" && get(db, "e") == "ve",
          "c=" + get(db, "c") + " d=" + get(db, "d") + " e=" + get(db, "e"));
    db->DeleteRange(wo, "c", "e");
    env.WaitIdle();
    check("range-bounds", get(db, "b") == "vb" && get(db, "c") == "<none>" && get(db, "d") == "<none>" &&
                              get(db, "e") == "ve",
          "b=" + get(db, "b") + " c=" + get(db, "c") + " d=" + get(db, "d") + " e=" + get(db, "e"));
    leveldb::WriteBatch b;
    b.Put("x1", "before");
    b.Merge("x3", "3,4");
    b.DeleteRange("x0", "x9");
    b.Put("x2", "after");
    b.Merge("x4", "5,6");
    st = db->Write(wo, &b);
    env.WaitIdle();
    std::string x4 = AffineFullMerge("x4", nullptr, {"5,6"});
    check("batch-order", st.ok() && get(db, "x1") == "<none>" && get(db, "x2") == "after" &&
                             get(db, "x3") == "<none>" && get(db, "x4") == x4,
          "x1=" + get(db, "x1") + " x2=" + get(db, "x2") + " x3=" + get(db, "x3") + " x4=" + get(db, "x4"));
    // a merge on a deleted key starts from nothing
    db->Put(wo, "m", "base");
    db->Delete(wo, "m");
    db->Merge(wo, "m", "7,8");
    env.WaitIdle();
    std::string want_m = AffineFullMerge("m", nullptr, {"7,8"});
    check("merge-after-delete", get(db, "m") == want_m, "m=" + get(db, "m"));
    // and survives a reopen and a full compaction
    delete db;
    env.WaitIdle();
    db = nullptr;
    st = open(path, true, &db);
    bool ok = st.ok();
    if (ok) {
      db->CompactRange(nullptr, nullptr);
      env.WaitIdle();
    }
    check("after-reopen-and-compaction",
          ok && get(db, "m") == want_m && get(db, "x2") == "after" && get(db, "c") == "<none>" &&
              get(db, "e") == "ve" && get(db, "x4") == x4,
          ok ? "m=" + get(db, "m") + " c=" + get(db, "c") : st.ToString());
    delete db;
    env.WaitIdle();
    DestroyScenarioDir(path);
  }
  return *passed == *total;
}

// ---- efficiency cases -------------------------------------------------------

namespace {

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

uint64_t TableBytes(const std::string& dir) {
  uint64_t total = 0;
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return 0;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    std::string n = e->d_name;
    if (n.size() > 4 && (n.compare(n.size() - 4, 4, ".ldb") == 0 || n.compare(n.size() - 4, 4, ".sst") == 0)) {
      struct stat st;
      if (stat((dir + "/" + n).c_str(), &st) == 0) total += st.st_size;
    }
  }
  closedir(d);
  return total;
}

void Metric(const char* name, uint64_t v) {
  printf("METRIC %s %llu\n", name, static_cast<unsigned long long>(v));
  fflush(stdout);
}

uint64_t CpuMicros() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return static_cast<uint64_t>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 +
         ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
}

struct Phase {
  uint64_t w0, r0;
  Phase() { SelfIo(&w0, &r0); }
  void Report(const char* wname, const char* rname) {
    uint64_t w1, r1;
    SelfIo(&w1, &r1);
    Metric(wname, w1 - w0);
    Metric(rname, r1 - r0);
  }
};

}  // namespace

bool RunPerfCase(const std::string& dir, const std::string& which, double scale,
                 std::string* error) {
  Db d;
  d.dir = dir;
  d.ext = true;
  // Stock sizes: 4 MB write buffer, 2 MB files, 4 KB blocks, a 1 MB block
  // cache so reads reach the files, no compression, bloom filters.
  d.shape = ConfShape{0, true, false, false, 4 << 20, 2 << 20, 4096};
  DestroyScenarioDir(dir);
  mkdir(dir.c_str(), 0755);
  d.cache.reset(leveldb::NewLRUCache(1 << 20));
  {
    leveldb::Options o;
    o.create_if_missing = true;
    o.env = &d.env;
    o.write_buffer_size = d.shape.write_buffer;
    o.max_file_size = d.shape.max_file;
    o.block_size = d.shape.block_size;
    o.compression = leveldb::kNoCompression;
    d.bloom.reset(leveldb::NewBloomFilterPolicy(10));
    o.filter_policy = d.bloom.get();
    o.block_cache = d.cache.get();
    o.merge_operator = &d.merge_op;
    leveldb::Status st = leveldb::DB::Open(o, dir, &d.db);
    d.env.WaitIdle();
    if (!st.ok()) {
      *error = "open: " + st.ToString();
      return false;
    }
  }
  auto reopen = [&]() -> bool {
    delete d.db;
    d.db = nullptr;
    d.env.WaitIdle();
    leveldb::Options o;
    o.env = &d.env;
    o.write_buffer_size = d.shape.write_buffer;
    o.max_file_size = d.shape.max_file;
    o.block_size = d.shape.block_size;
    o.compression = leveldb::kNoCompression;
    o.filter_policy = d.bloom.get();
    o.block_cache = d.cache.get();
    o.merge_operator = &d.merge_op;
    leveldb::Status st = leveldb::DB::Open(o, dir, &d.db);
    d.env.WaitIdle();
    if (!st.ok()) *error = "reopen: " + st.ToString();
    return st.ok();
  };
  ConfRng rng(0x9E3779B9ull ^ static_cast<uint64_t>(scale * 1000));
  ConfModel model;
  leveldb::WriteOptions wo;
  auto check_gets = [&](uint64_t n, uint64_t keys, const char* where) -> bool {
    bool ok = CheckGets(&d, leveldb::ReadOptions(), model, &rng, keys, n, where);
    if (!ok) *error = d.error;
    return ok;
  };
  auto count_scan = [&](const char* where) -> bool {
    bool ok = CheckScan(&d, leveldb::ReadOptions(), model, where);
    if (!ok) *error = d.error;
    return ok;
  };

  bool ok = true;
  if (which == "rangedel") {
    // Load, compacted to the bottom.  Phase A: 400 range deletes clear 90%
    // of the keys in runs of every size.  Phase S: full scans both ways and
    // point reads while the tombstones are still in the memtable.  Phase R:
    // reopen and let background compactions run (no compaction asked for).
    // Phase B: a full compaction.
    const uint64_t keys = static_cast<uint64_t>(300000 * scale);
    for (uint64_t id = 0; id < keys && ok; id++) {
      std::string v = ConfValue(id, 1, 90);
      leveldb::Status st = d.db->Put(wo, KeyName(id), v);
      if (!st.ok()) { *error = "load: " + st.ToString(); ok = false; }
      model[id] = v;
    }
    d.env.WaitIdle();
    if (!ok) { d.Close(); return false; }
    d.db->CompactRange(nullptr, nullptr);
    d.env.WaitIdle();
    Metric("LOAD_TABLE_BYTES", TableBytes(dir));
    {
      Phase a;
      const uint64_t ranges = 400;
      const uint64_t stride = keys / ranges;
      for (uint64_t i = 0; i < ranges && ok; i++) {
        // cover 90% of each stride, at a random offset
        uint64_t lo = i * stride + rng.Below(stride / 10 + 1);
        uint64_t hi = lo + stride * 9 / 10;
        leveldb::Status st = d.db->DeleteRange(wo, KeyName(lo), KeyName(hi));
        if (!st.ok()) { *error = "DeleteRange: " + st.ToString(); ok = false; }
        model.erase(model.lower_bound(lo), model.lower_bound(hi));
      }
      d.env.WaitIdle();
      a.Report("A_WCHAR", "A_RCHAR");
    }
    if (ok) {
      Phase sc;
      ok = count_scan("scans across the deleted ranges") &&
           check_gets(2000, keys, "reads across the deleted ranges");
      sc.Report("S_WCHAR", "S_RCHAR");
    }
    if (ok) {
      Phase r;
      ok = reopen();
      d.env.WaitIdle();
      r.Report("R_WCHAR", "R_RCHAR");
      Metric("R_TABLE_BYTES", TableBytes(dir));
    }
    ok = ok && check_gets(4000, keys, "reads after reopen") && count_scan("scan after reopen");
    if (ok) {
      Phase b;
      d.db->CompactRange(nullptr, nullptr);
      d.env.WaitIdle();
      b.Report("B_WCHAR", "B_RCHAR");
      Metric("B_TABLE_BYTES", TableBytes(dir));
      ok = count_scan("scan after compaction") && check_gets(4000, keys, "reads after compaction");
    }
    Metric("SURVIVORS", model.size());
  } else if (which == "manyranges") {
    // 100,000 small range deletes, each followed by point reads, with an
    // iterator walk every 1000: the cost of a read must not grow with the
    // number of tombstones.  Measured in CPU time (all threads).
    const uint64_t keys = static_cast<uint64_t>(100000 * scale);
    const uint64_t ranges = static_cast<uint64_t>(100000 * scale);
    for (uint64_t id = 0; id < keys && ok; id++) {
      std::string v = ConfValue(id, 1, 60);
      leveldb::Status st = d.db->Put(wo, KeyName(id), v);
      if (!st.ok()) { *error = "load: " + st.ToString(); ok = false; }
      model[id] = v;
    }
    d.env.WaitIdle();
    if (!ok) { d.Close(); return false; }
    d.db->CompactRange(nullptr, nullptr);
    d.env.WaitIdle();
    Metric("LOAD_TABLE_BYTES", TableBytes(dir));
    uint64_t cpu0 = CpuMicros();
    for (uint64_t i = 0; i < ranges && ok; i++) {
      uint64_t lo = rng.Below(keys * 2);  // half of them over nothing
      uint64_t hi = lo + 1 + rng.Below(3);
      leveldb::Status st = d.db->DeleteRange(wo, KeyName(lo), KeyName(hi));
      if (!st.ok()) { *error = "DeleteRange: " + st.ToString(); ok = false; break; }
      model.erase(model.lower_bound(lo), model.lower_bound(hi));
      if (i % 4 == 0) {
        // now and then something comes back
        uint64_t id = rng.Below(keys);
        std::string v = ConfValue(id, 2 + i, 60);
        st = d.db->Put(wo, KeyName(id), v);
        if (!st.ok()) { *error = "Put: " + st.ToString(); ok = false; break; }
        model[id] = v;
      }
      ok = check_gets(2, keys, "reads between range deletes");
      if (ok && i % 1000 == 999) {
        ok = CheckWalk(&d, leveldb::ReadOptions(), model, rng.Below(keys), 20, &rng);
        if (!ok) *error = d.error;
      }
    }
    d.env.WaitIdle();
    Metric("M_CPU_MS", (CpuMicros() - cpu0) / 1000);
    ok = ok && count_scan("scan after the range deletes") && reopen() && count_scan("scan after reopen");
    if (ok) {
      d.db->CompactRange(nullptr, nullptr);
      d.env.WaitIdle();
      ok = count_scan("scan after compaction");
    }
    Metric("SURVIVORS", model.size());
  } else if (which == "merge") {
    // 20k keys with 400-byte values, compacted; then 400k merge operands
    // spread over them (phase A); reads; then a full compaction (phase B)
    // after which every key holds one plain value again.
    const uint64_t keys = static_cast<uint64_t>(20000 * scale);
    const uint64_t merges = static_cast<uint64_t>(400000 * scale);
    for (uint64_t id = 0; id < keys && ok; id++) {
      std::string v = ConfValue(id, 1, 400);
      leveldb::Status st = d.db->Put(wo, KeyName(id), v);
      if (!st.ok()) { *error = "load: " + st.ToString(); ok = false; }
      model[id] = v;
    }
    d.env.WaitIdle();
    if (!ok) { d.Close(); return false; }
    d.db->CompactRange(nullptr, nullptr);
    d.env.WaitIdle();
    Metric("LOAD_TABLE_BYTES", TableBytes(dir));
    {
      Phase a;
      for (uint64_t i = 0; i < merges && ok; i++) {
        uint64_t id = rng.Below(keys);
        std::string operand = AffineOperand(1 + rng.Below(1000), rng.Below(1000));
        leveldb::Status st = d.db->Merge(wo, KeyName(id), operand);
        if (!st.ok()) { *error = "Merge: " + st.ToString(); ok = false; }
        auto it = model.find(id);
        model[id] = AffineFullMerge(KeyName(id), it == model.end() ? nullptr : &it->second, {operand});
      }
      d.env.WaitIdle();
      a.Report("A_WCHAR", "A_RCHAR");
    }
    ok = ok && check_gets(4000, keys, "reads after the merges");
    Metric("A_TABLE_BYTES", TableBytes(dir));
    if (ok) {
      Phase b;
      d.db->CompactRange(nullptr, nullptr);
      d.env.WaitIdle();
      b.Report("B_WCHAR", "B_RCHAR");
      Metric("B_TABLE_BYTES", TableBytes(dir));
      ok = reopen() && count_scan("scan after compaction") && check_gets(4000, keys, "reads after compaction");
    }
    if (ok) {
      Phase c;
      ok = check_gets(4000, keys, "point reads after compaction");
      c.Report("C_WCHAR", "C_RCHAR");
    }
  } else {
    *error = "unknown case " + which;
    ok = false;
  }
  d.Close();
  // peak resident memory of the whole case
  FILE* f = fopen("/proc/self/status", "r");
  if (f != nullptr) {
    char line[256];
    while (fgets(line, sizeof(line), f) != nullptr) {
      unsigned long long kb = 0;
      if (sscanf(line, "VmHWM: %llu kB", &kb) == 1) Metric("VMHWM_KB", kb);
    }
    fclose(f);
  }
  return ok;
}

#endif  // CONF_PRISTINE

}  // namespace lsmbench
