#include "conf_model.h"

#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <algorithm>
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
  for (uint32_t i = 0; i < kMaxFamilies; i++) {
    s.family_write_buffer[i] =
        i == 0 ? s.write_buffer
               : static_cast<uint32_t>(24 * 1024 + r.Below(72 * 1024));
  }
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

// One of the families that exist right now, the default one most often.
uint32_t ScenarioGen::PickFamily(const ConfModels& models) {
  if (!ext_) return 0;
  uint32_t live[kMaxFamilies];
  uint32_t n = 0;
  for (uint32_t i = 0; i < kMaxFamilies; i++) {
    if (live_[i]) live[n++] = i;
  }
  if (n == 1 || rng_.Chance(40)) return 0;
  return live[rng_.Below(n)];
}

bool ScenarioGen::Next(const ConfModels& models, ConfOp* op) {
  *op = ConfOp();
  if (done_ >= ops_) return false;
  done_++;
  const uint64_t keys = shape_.keys;
  uint64_t roll = rng_.Below(100);
  if (ext_ && roll < 4) {
    // create or drop a column family (durable, so it counts as a write)
    uint32_t candidates[kMaxFamilies];
    uint32_t n = 0;
    const bool create = rng_.Chance(60);
    for (uint32_t i = 1; i < kMaxFamilies; i++) {
      if (live_[i] != create) candidates[n++] = i;
    }
    if (n > 0) {
      op->family = candidates[rng_.Below(n)];
      op->kind = create ? kOpCreate : kOpDrop;
      live_[op->family] = create;
      if (!create) snaps_open_ = 0;  // snapshots are checked per family
      return true;
    }
    roll = 50;  // nothing to create or drop: fall through to a read
  }
  const uint32_t family = PickFamily(models);
  op->family = family;
  const ConfModel& model = models.m[family];
  if (roll < 12) {
    // a single put or delete
    op->kind = kOpWrite;
    op->single = true;
    ConfWrite w;
    RandomWrite(&w, rng_.Below(keys), false);
    w.family = family;
    op->writes.push_back(w);
  } else if (roll < 30 && !(ext_ && roll >= 20)) {
    // a batch of puts and deletes (and merges), sometimes spanning families
    op->kind = kOpWrite;
    uint64_t n = 1 + rng_.Below(rng_.Chance(10) ? 200 : 12);
    const bool span = ext_ && rng_.Chance(30);
    for (uint64_t i = 0; i < n; i++) {
      ConfWrite w;
      RandomWrite(&w, rng_.Below(keys), ext_);
      w.family = span ? PickFamily(models) : family;
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
    w.family = family;
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
    w.family = family;
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
        w.family = family;
        op->writes.push_back(w);
      }
      ConfWrite d;
      d.kind = kWDeleteRange;
      d.id = lo;
      d.end = hi;
      d.family = family;
      op->writes.push_back(d);
      uint64_t after = rng_.Below(4);
      for (uint64_t i = 0; i < after; i++) {
        ConfWrite w;
        RandomWrite(&w, lo + rng_.Below(hi - lo + 2), true);
        w.family = family;
        op->writes.push_back(w);
      }
      if (rng_.Chance(20)) {
        ConfWrite d2;
        d2.kind = kWDeleteRange;
        d2.id = lo + rng_.Below(hi - lo);
        d2.end = d2.id + rng_.Below(keys / 8 + 1);
        d2.family = family;
        op->writes.push_back(d2);
      }
    } else {
      for (auto it = model.lower_bound(lo); it != model.end() && it->first < hi; ++it) {
        op->writes.push_back({kWDelete, it->first, 0, "", family});
      }
      uint64_t back = rng_.Below(4);
      for (uint64_t i = 0; i < back; i++) {
        uint64_t id = lo + rng_.Below(hi - lo);
        op->writes.push_back(
            {kWPut, id, 0, ConfValue(id, version_++, rng_.Below(160)), family});
      }
      if (op->writes.empty()) op->writes.push_back({kWDelete, lo, 0, "", family});
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
      w.family = family;
      op->writes.push_back(w);
    }
  }
  return true;
}

void ScenarioGen::Apply(const ConfOp& op, ConfModels* models) {
  if (op.kind == kOpCreate) {
    models->live[op.family] = true;
    models->m[op.family].clear();
    return;
  }
  if (op.kind == kOpDrop) {
    models->live[op.family] = false;
    models->m[op.family].clear();
    return;
  }
  if (op.kind != kOpWrite) return;
  for (const ConfWrite& w : op.writes) {
    ConfModel* model = &models->m[w.family];
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
        std::string v = AffineFullMerge(ConfKey(w.id),
                                        it == model->end() ? nullptr : &it->second, ops);
        (*model)[w.id] = v;
        break;
      }
      case kWDeleteRange:
        if (w.id < w.end) model->erase(model->lower_bound(w.id), model->lower_bound(w.end));
        break;
    }
  }
}

ConfModels ModelsAfterWrites(uint64_t seed, uint64_t ops, bool ext, uint64_t writes,
                             bool* past_end) {
  ScenarioGen gen(seed, ops, ext);
  ConfModels models;
  ConfOp op;
  uint64_t n = 0;
  *past_end = false;
  while (n < writes) {
    if (!gen.Next(models, &op)) {
      *past_end = true;
      break;
    }
    if (op.kind != kOpWrite && op.kind != kOpCreate && op.kind != kOpDrop) continue;
    ScenarioGen::Apply(op, &models);
    n++;
  }
  return models;
}

void DestroyScenarioDir(const std::string& dir) {
  // Remove every file and every column family's directory, not only the
  // ones the engine knows about.
  DIR* d = opendir(dir.c_str());
  if (d != nullptr) {
    struct dirent* e;
    std::vector<std::string> subdirs;
    while ((e = readdir(d)) != nullptr) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      const std::string path = dir + "/" + e->d_name;
      struct stat st;
      if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        subdirs.push_back(path);
      } else {
        unlink(path.c_str());
      }
    }
    closedir(d);
    for (const std::string& sub : subdirs) DestroyScenarioDir(sub);
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
  // One handle per family index; null when that family does not exist.
  leveldb::ColumnFamilyHandle* handles[kMaxFamilies] = {nullptr, nullptr, nullptr,
                                                        nullptr};
#endif
  bool live[kMaxFamilies] = {true, false, false, false};

  bool Fail(const std::string& what) {
    if (error.empty()) error = what;
    return false;
  }

  leveldb::Options OptionsFor(uint32_t family) {
    leveldb::Options o;
    o.paranoid_checks = true;
    o.env = &env;
    o.write_buffer_size = shape.family_write_buffer[family];
    o.max_file_size = shape.max_file;
    o.block_size = family % 2 == 1 ? 1024 : shape.block_size;
    o.compression = leveldb::kNoCompression;
    o.reuse_logs = shape.reuse_logs;
    if (shape.bloom || family % 3 == 1) {
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
    return o;
  }

  // Opens with one descriptor per family in `live`.
  bool Open(bool create) {
    leveldb::Options o = OptionsFor(0);
    o.create_if_missing = create;
#ifndef CONF_PRISTINE
    if (ext) {
      std::vector<leveldb::ColumnFamilyDescriptor> families;
      std::vector<uint32_t> order;
      for (uint32_t i = 0; i < kMaxFamilies; i++) {
        if (!live[i]) continue;
        families.emplace_back(ConfFamilyName(i), OptionsFor(i));
        order.push_back(i);
      }
      std::vector<leveldb::ColumnFamilyHandle*> got;
      leveldb::Status st = leveldb::DB::Open(o, dir, families, &got, &db);
      env.WaitIdle();
      if (!st.ok()) return Fail("open: " + st.ToString());
      for (size_t i = 0; i < order.size(); i++) handles[order[i]] = got[i];
      return true;
    }
#endif
    leveldb::Status st = leveldb::DB::Open(o, dir, &db);
    env.WaitIdle();
    if (!st.ok()) return Fail("open: " + st.ToString());
    return true;
  }

  void Close() {
    if (db != nullptr) env.WaitIdle();
#ifndef CONF_PRISTINE
    for (uint32_t i = 0; i < kMaxFamilies; i++) {
      if (handles[i] != nullptr && db != nullptr) {
        db->DestroyColumnFamilyHandle(handles[i]);
      }
      handles[i] = nullptr;
    }
#endif
    delete db;
    db = nullptr;
    env.WaitIdle();
  }

#ifndef CONF_PRISTINE
  leveldb::ColumnFamilyHandle* Handle(uint32_t family) {
    return family == 0 ? nullptr : handles[family];
  }

  bool CreateFamily(uint32_t family) {
    leveldb::ColumnFamilyHandle* h = nullptr;
    leveldb::Status st =
        db->CreateColumnFamily(OptionsFor(family), ConfFamilyName(family), &h);
    env.WaitIdle();
    if (!st.ok()) return Fail("CreateColumnFamily " + ConfFamilyName(family) + ": " + st.ToString());
    handles[family] = h;
    live[family] = true;
    return true;
  }

  bool DropFamily(uint32_t family) {
    leveldb::Status st = db->DropColumnFamily(handles[family]);
    env.WaitIdle();
    if (!st.ok()) return Fail("DropColumnFamily " + ConfFamilyName(family) + ": " + st.ToString());
    st = db->DestroyColumnFamilyHandle(handles[family]);
    env.WaitIdle();
    if (!st.ok()) return Fail("DestroyColumnFamilyHandle: " + st.ToString());
    handles[family] = nullptr;
    live[family] = false;
    return true;
  }
#endif

  leveldb::Status Get(const leveldb::ReadOptions& ro, uint32_t family,
                      const std::string& key, std::string* value) {
#ifndef CONF_PRISTINE
    if (family != 0) return db->Get(ro, handles[family], key, value);
#endif
    return db->Get(ro, key, value);
  }

  leveldb::Iterator* NewIterator(const leveldb::ReadOptions& ro, uint32_t family) {
#ifndef CONF_PRISTINE
    if (family != 0) return db->NewIterator(ro, handles[family]);
#endif
    return db->NewIterator(ro);
  }

  void CompactRange(uint32_t family, const leveldb::Slice* b, const leveldb::Slice* e) {
#ifndef CONF_PRISTINE
    if (family != 0) {
      db->CompactRange(handles[family], b, e);
      return;
    }
#endif
    db->CompactRange(b, e);
  }
};

std::string KeyName(uint64_t id) { return ConfKey(id); }

bool CheckGets(Db* d, const leveldb::ReadOptions& ro, uint32_t family,
               const ConfModel& model, ConfRng* rng, uint64_t keys, uint64_t n,
               const char* where) {
  for (uint64_t i = 0; i < n; i++) {
    uint64_t id = rng->Below(keys + 20);
    std::string got;
    leveldb::Status st = d->Get(ro, family, KeyName(id), &got);
    d->env.WaitIdle();
    auto it = model.find(id);
    if (it == model.end()) {
      if (st.IsNotFound()) continue;
      if (!st.ok()) return d->Fail(std::string(where) + ": Get " + KeyName(id) + ": " + st.ToString());
      return d->Fail(std::string(where) + " [" + ConfFamilyName(family) + "]: Get " +
                     KeyName(id) + " found '" + got + "', expected NotFound");
    }
    if (!st.ok()) return d->Fail(std::string(where) + ": Get " + KeyName(id) + ": " + st.ToString() +
                                 ", expected '" + it->second + "'");
    if (got != it->second) {
      return d->Fail(std::string(where) + " [" + ConfFamilyName(family) + "]: Get " +
                     KeyName(id) + " returned '" + got + "', expected '" + it->second + "'");
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

bool CheckScan(Db* d, const leveldb::ReadOptions& ro, uint32_t family,
               const ConfModel& model, const char* where) {
  std::unique_ptr<leveldb::Iterator> it(d->NewIterator(ro, family));
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
bool CheckWalk(Db* d, const leveldb::ReadOptions& ro, uint32_t family,
               const ConfModel& model, uint64_t start, uint64_t steps,
               ConfRng* rng) {
  std::unique_ptr<leveldb::Iterator> it(d->NewIterator(ro, family));
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
#ifndef CONF_PRISTINE
  if (op.single) {
    const ConfWrite& w = op.writes[0];
    leveldb::ColumnFamilyHandle* h = d->Handle(w.family);
    switch (w.kind) {
      case kWPut:
        return h == nullptr ? d->db->Put(wo, KeyName(w.id), w.value)
                            : d->db->Put(wo, h, KeyName(w.id), w.value);
      case kWDelete:
        return h == nullptr ? d->db->Delete(wo, KeyName(w.id))
                            : d->db->Delete(wo, h, KeyName(w.id));
      case kWMerge:
        return h == nullptr ? d->db->Merge(wo, KeyName(w.id), w.value)
                            : d->db->Merge(wo, h, KeyName(w.id), w.value);
      case kWDeleteRange:
        return h == nullptr
                   ? d->db->DeleteRange(wo, KeyName(w.id), KeyName(w.end))
                   : d->db->DeleteRange(wo, h, KeyName(w.id), KeyName(w.end));
    }
  }
  leveldb::WriteBatch batch;
  for (const ConfWrite& w : op.writes) {
    leveldb::ColumnFamilyHandle* h = d->Handle(w.family);
    switch (w.kind) {
      case kWPut:
        if (h == nullptr) batch.Put(KeyName(w.id), w.value);
        else batch.Put(h, KeyName(w.id), w.value);
        break;
      case kWDelete:
        if (h == nullptr) batch.Delete(KeyName(w.id));
        else batch.Delete(h, KeyName(w.id));
        break;
      case kWMerge:
        if (h == nullptr) batch.Merge(KeyName(w.id), w.value);
        else batch.Merge(h, KeyName(w.id), w.value);
        break;
      case kWDeleteRange:
        if (h == nullptr) batch.DeleteRange(KeyName(w.id), KeyName(w.end));
        else batch.DeleteRange(h, KeyName(w.id), KeyName(w.end));
        break;
    }
  }
  return d->db->Write(wo, &batch);
#else
  if (op.single) {
    const ConfWrite& w = op.writes[0];
    switch (w.kind) {
      case kWPut:
        return d->db->Put(wo, KeyName(w.id), w.value);
      case kWDelete:
        return d->db->Delete(wo, KeyName(w.id));
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
      default:
        return leveldb::Status::NotSupported("extension write in a stock scenario");
    }
  }
  return d->db->Write(wo, &batch);
#endif
}

std::string DescribeWrite(const ConfOp& op) {
  std::string s = op.single ? "single " : "batch of " + std::to_string(op.writes.size()) + " ";
  const ConfWrite& w = op.writes[0];
  const char* names[] = {"Put", "Delete", "Merge", "DeleteRange"};
  s += names[w.kind];
  s += " " + KeyName(w.id) + " in " + ConfFamilyName(w.family);
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
  ConfModels models;
  struct Snap {
    const leveldb::Snapshot* snap;
    ConfModels models;
  };
  std::vector<Snap> snaps;
  uint64_t writes = 0, opno = 0;
  ConfOp op;
  bool ok = true;
  // Reads of every family the snapshot saw.
  auto check_snapshot = [&](const Snap& sn, const char* where) {
    for (uint32_t f = 0; f < kMaxFamilies && ok; f++) {
      if (!sn.models.live[f] || !d.live[f]) continue;
      leveldb::ReadOptions ro;
      ro.snapshot = sn.snap;
      ok = CheckGets(&d, ro, f, sn.models.m[f], &check_rng, gen.keys(), 16, where) &&
           CheckScan(&d, ro, f, sn.models.m[f], where);
    }
    return ok;
  };
  auto check_all = [&](const char* where, uint64_t gets) {
    for (uint32_t f = 0; f < kMaxFamilies && ok; f++) {
      if (!d.live[f]) continue;
      leveldb::ReadOptions ro;
      ok = CheckGets(&d, ro, f, models.m[f], &check_rng, gen.keys(), gets, where);
    }
    return ok;
  };
  while (ok && gen.Next(models, &op)) {
    opno++;
    leveldb::ReadOptions ro;
    std::string where = "op " + std::to_string(opno);
    const uint32_t family = op.family;
    const ConfModel& model = models.m[family];
    switch (op.kind) {
      case kOpCreate:
#ifndef CONF_PRISTINE
        ok = d.CreateFamily(family);
        if (ok) {
          ScenarioGen::Apply(op, &models);
          WriteAck(ack_fd, ++writes);
        }
#endif
        break;
      case kOpDrop:
#ifndef CONF_PRISTINE
        // The snapshots' views of a dropped family go away with it.
        for (Snap& sn : snaps) sn.models.live[family] = false;
        ok = d.DropFamily(family);
        if (ok) {
          ScenarioGen::Apply(op, &models);
          WriteAck(ack_fd, ++writes);
        }
#endif
        break;
      case kOpWrite: {
        leveldb::Status st = DoWrite(&d, op);
        d.env.WaitIdle();
        if (!st.ok()) { ok = d.Fail(where + ": write (" + DescribeWrite(op) + "): " + st.ToString()); break; }
        ScenarioGen::Apply(op, &models);
        WriteAck(ack_fd, ++writes);
        break;
      }
      case kOpGets:
        ok = CheckGets(&d, ro, family, model, &check_rng, gen.keys(), op.count,
                       (where + " reads").c_str());
        break;
      case kOpWalk:
        ok = CheckWalk(&d, ro, family, model, op.id, op.count, &check_rng);
        break;
      case kOpScan:
        ok = CheckScan(&d, ro, family, model, (where + " scan").c_str());
        break;
      case kOpSnapTake:
        snaps.push_back({d.db->GetSnapshot(), models});
        break;
      case kOpSnapCheck:
        if (op.id < snaps.size()) {
          ok = check_snapshot(snaps[op.id], (where + " snapshot").c_str());
          d.db->ReleaseSnapshot(snaps[op.id].snap);
          snaps.erase(snaps.begin() + op.id);
        }
        break;
      case kOpReopen:
        for (Snap& sn : snaps) d.db->ReleaseSnapshot(sn.snap);
        snaps.clear();
        d.Close();
        ok = d.Open(false) && check_all((where + " after reopen").c_str(), 24);
        break;
      case kOpCompact:
        if (op.count == 0) {
          d.CompactRange(family, nullptr, nullptr);
        } else {
          std::string lo = KeyName(op.id), hi = KeyName(op.id + op.count);
          leveldb::Slice b(lo), e(hi);
          d.CompactRange(family, &b, &e);
        }
        d.env.WaitIdle();
        ok = check_all((where + " after compaction").c_str(), 24);
        if (ok && !snaps.empty()) {
          // what the compaction kept for the oldest snapshot
          ok = check_snapshot(snaps[0], (where + " snapshot after compaction").c_str());
        }
        break;
    }
  }
  for (size_t i = 0; ok && i < snaps.size(); i++) {
    ok = check_snapshot(snaps[i], "final snapshot");
  }
  for (Snap& sn : snaps) d.db->ReleaseSnapshot(sn.snap);
  for (uint32_t f = 0; ok && f < kMaxFamilies; f++) {
    if (d.live[f]) ok = CheckScan(&d, leveldb::ReadOptions(), f, models.m[f], "final scan");
  }
  if (ok) {
    d.Close();
    ok = d.Open(false);
    for (uint32_t f = 0; ok && f < kMaxFamilies; f++) {
      if (d.live[f]) ok = CheckScan(&d, leveldb::ReadOptions(), f, models.m[f], "final reopen");
    }
  }
  if (ok) {
    // compacted all the way down, nothing may change
    for (uint32_t f = 0; f < kMaxFamilies; f++) {
      if (d.live[f]) d.CompactRange(f, nullptr, nullptr);
    }
    d.env.WaitIdle();
    for (uint32_t f = 0; ok && f < kMaxFamilies; f++) {
      if (d.live[f]) {
        ok = CheckScan(&d, leveldb::ReadOptions(), f, models.m[f], "after final compaction");
      }
    }
  }
  d.Close();
  if (!ok) *error = d.error;
  return ok;
}

bool CheckRecovered(const std::string& dir, uint64_t seed, uint64_t ops, bool ext,
                    uint64_t acked, uint64_t* recovered, std::string* error) {
  bool end0 = false, end1 = false;
  ConfModels m0 = ModelsAfterWrites(seed, ops, ext, acked, &end0);
  ConfModels m1 = ModelsAfterWrites(seed, ops, ext, acked + 1, &end1);
  // Does the database hold the acknowledged state, or the one the write in
  // flight would have made?  The families that exist decide which set to
  // open with, so both are tried in turn.
  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt == 1 && end1) break;
    ConfModels& m = attempt == 0 ? m0 : m1;
    Db d;
    d.dir = dir;
    d.ext = ext;
    d.shape = ShapeFor(seed);
    for (uint32_t f = 0; f < kMaxFamilies; f++) d.live[f] = m.live[f];
    // With nothing acknowledged the crash may have come before the
    // database existed, so it may be created here.
    if (!d.Open(acked == 0 && attempt == 0)) {
      if (attempt == 0) continue;  // maybe the family set of acked + 1
      *error = d.error;
      return false;
    }
    bool match = true;
    std::string first_error;
    for (uint32_t f = 0; f < kMaxFamilies && match; f++) {
      if (!m.live[f]) continue;
      const std::string what = "recovered " + ConfFamilyName(f);
      match = CheckScan(&d, leveldb::ReadOptions(), f, m.m[f], what.c_str());
      if (!match) first_error = d.error;
      d.error.clear();
    }
    if (!match) {
      d.Close();
      if (attempt == 0) continue;
      *error = "recovered state matches neither " + std::to_string(acked) + " nor " +
               std::to_string(acked + 1) + " writes: " + first_error;
      return false;
    }
    *recovered = acked + attempt;
    // The database has to be usable afterwards: write, reopen, read back.
    ConfRng rng(seed ^ 0xAF7Eull);
    for (uint64_t i = 0; i < 60; i++) {
      ConfOp op;
      op.kind = kOpWrite;
      op.single = true;
      uint64_t id = rng.Below(d.shape.keys);
      uint32_t f = 0;
      if (ext) {
        uint32_t live[kMaxFamilies];
        uint32_t n = 0;
        for (uint32_t j = 0; j < kMaxFamilies; j++) {
          if (m.live[j]) live[n++] = j;
        }
        f = live[rng.Below(n)];
      }
      ConfWrite w{kWPut, id, 0, ConfValue(id, 1000000000ull + i, 40), f};
      if (ext && i % 3 == 1) {
        w = ConfWrite{kWMerge, id, 0,
                      AffineOperand(1 + rng.Below(kAffineP - 1), rng.Below(kAffineP)), f};
      } else if (ext && i % 10 == 9) {
        w = ConfWrite{kWDeleteRange, id, id + 1 + rng.Below(30), "", f};
      }
      op.writes.push_back(w);
      op.family = f;
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
    bool ok = d.Open(false);
    for (uint32_t f = 0; ok && f < kMaxFamilies; f++) {
      if (m.live[f]) {
        ok = CheckScan(&d, leveldb::ReadOptions(), f, m.m[f], "after recovery and reopen");
      }
    }
    d.Close();
    if (!ok) *error = d.error;
    return ok;
  }
  *error = "recovered state matches neither " + std::to_string(acked) + " nor " +
           std::to_string(acked + 1) + " writes";
  return false;
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
  // 4. Column families: separate key spaces, per-family options, atomic
  //    batches across them, drop and re-create, and what a reopen needs.
  {
    std::string path = dir + "/families";
    DestroyScenarioDir(path);
    mkdir(path.c_str(), 0755);
    leveldb::Options base;
    base.create_if_missing = true;
    base.env = &env;
    base.write_buffer_size = 64 * 1024;
    base.compression = leveldb::kNoCompression;
    leveldb::Options with_merge = base;
    with_merge.merge_operator = &op;

    leveldb::DB* db = nullptr;
    leveldb::Status st = leveldb::DB::Open(with_merge, path, &db);
    env.WaitIdle();
    if (!st.ok()) {
      check("open", false, st.ToString());
      return false;
    }
    leveldb::ColumnFamilyHandle* def = db->DefaultColumnFamily();
    check("default-family", def != nullptr && def->GetName() == "default" &&
                                def->GetID() == 0,
          def == nullptr ? "no default handle" : def->GetName());

    // a family with a merge operator and one without
    leveldb::ColumnFamilyHandle* a = nullptr;
    leveldb::ColumnFamilyHandle* b = nullptr;
    st = db->CreateColumnFamily(with_merge, "alpha", &a);
    leveldb::Status st2 = db->CreateColumnFamily(base, "beta", &b);
    env.WaitIdle();
    check("create-family", st.ok() && st2.ok() && a != nullptr && b != nullptr &&
                               a->GetID() != 0 && a->GetID() != b->GetID(),
          st.ToString() + "/" + st2.ToString());
    st = db->CreateColumnFamily(base, "alpha", nullptr);
    check("create-duplicate", st.IsInvalidArgument(), st.ToString());

    // the same key in three families is three entries
    db->Put(leveldb::WriteOptions(), "k", "in-default");
    db->Put(leveldb::WriteOptions(), a, "k", "in-alpha");
    db->Put(leveldb::WriteOptions(), b, "k", "in-beta");
    env.WaitIdle();
    auto get_cf = [&](leveldb::ColumnFamilyHandle* h) {
      std::string v;
      leveldb::Status g = h == nullptr ? db->Get(leveldb::ReadOptions(), "k", &v)
                                       : db->Get(leveldb::ReadOptions(), h, "k", &v);
      env.WaitIdle();
      return g.ok() ? v : (g.IsNotFound() ? std::string("<none>") : "<" + g.ToString() + ">");
    };
    check("separate-key-spaces",
          get_cf(nullptr) == "in-default" && get_cf(a) == "in-alpha" &&
              get_cf(b) == "in-beta",
          get_cf(nullptr) + "/" + get_cf(a) + "/" + get_cf(b));

    // one batch, three families, and an iterator that sees only its own
    {
      leveldb::WriteBatch batch;
      batch.Put("d1", "x");
      batch.Put(a, "a1", "y");
      batch.Merge(a, "a1", "3,4");
      batch.DeleteRange(b, "k", "l");
      batch.Delete(a, "gone");
      st = db->Write(leveldb::WriteOptions(), &batch);
      env.WaitIdle();
      std::string want_a1 = AffineFullMerge("a1", nullptr, {"3,4"});
      std::string v;
      bool ok_a = db->Get(leveldb::ReadOptions(), a, "a1", &v).ok();
      std::string got_a1 = ok_a ? v : "<none>";
      // the merge above starts from "y"
      std::string base_y = "y";
      want_a1 = AffineFullMerge("a1", &base_y, {"3,4"});
      env.WaitIdle();
      size_t seen = 0;
      std::unique_ptr<leveldb::Iterator> it(db->NewIterator(leveldb::ReadOptions(), a));
      for (it->SeekToFirst(); it->Valid(); it->Next()) seen++;
      it.reset();
      env.WaitIdle();
      check("batch-across-families",
            st.ok() && got_a1 == want_a1 && get_cf(b) == "<none>" &&
                get_cf(nullptr) == "in-default" && seen == 2,
            st.ToString() + " a1=" + got_a1 + " beta k=" + get_cf(b) +
                " alpha keys=" + std::to_string(seen));
    }

    // a merge in a family without an operator is refused, and nothing of
    // the batch is applied
    st = db->Merge(leveldb::WriteOptions(), b, "m", "1,1");
    env.WaitIdle();
    bool refused = st.IsInvalidArgument();
    {
      leveldb::WriteBatch batch;
      batch.Put(b, "b-put", "v");
      batch.Merge(b, "m", "1,1");
      leveldb::Status w = db->Write(leveldb::WriteOptions(), &batch);
      env.WaitIdle();
      std::string v;
      bool applied = db->Get(leveldb::ReadOptions(), b, "b-put", &v).ok();
      env.WaitIdle();
      check("per-family-merge-operator",
            refused && w.IsInvalidArgument() && !applied,
            st.ToString() + "/" + w.ToString() + (applied ? " applied" : ""));
    }

    // a snapshot spans the families
    const leveldb::Snapshot* snap = db->GetSnapshot();
    db->Put(leveldb::WriteOptions(), "k", "later");
    db->Put(leveldb::WriteOptions(), a, "k", "later");
    env.WaitIdle();
    {
      leveldb::ReadOptions ro;
      ro.snapshot = snap;
      std::string v1, v2;
      db->Get(ro, "k", &v1);
      db->Get(ro, a, "k", &v2);
      env.WaitIdle();
      check("snapshot-across-families", v1 == "in-default" && v2 == "in-alpha",
            v1 + "/" + v2);
    }
    db->ReleaseSnapshot(snap);

    // reopening needs every family named
    const uint32_t alpha_id = a->GetID();
    db->DestroyColumnFamilyHandle(a);
    db->DestroyColumnFamilyHandle(b);
    delete db;
    env.WaitIdle();
    db = nullptr;
    std::vector<std::string> names;
    st = leveldb::DB::ListColumnFamilies(base, path, &names);
    std::sort(names.begin(), names.end());
    check("list-families",
          st.ok() && names.size() == 3 && names[0] == "alpha" &&
              names[1] == "beta" && names[2] == "default",
          st.ToString() + " [" + Join(names) + "]");

    leveldb::Options plain = base;
    plain.create_if_missing = false;
    st = leveldb::DB::Open(plain, path, &db);
    env.WaitIdle();
    check("open-without-families", st.IsInvalidArgument(), st.ToString());
    if (st.ok()) { delete db; db = nullptr; env.WaitIdle(); }

    std::vector<leveldb::ColumnFamilyDescriptor> descs;
    descs.emplace_back("default", with_merge);
    descs.emplace_back("alpha", with_merge);
    std::vector<leveldb::ColumnFamilyHandle*> handles;
    st = leveldb::DB::Open(plain, path, descs, &handles, &db);
    env.WaitIdle();
    check("open-missing-one-family", st.IsInvalidArgument(), st.ToString());
    if (st.ok()) { delete db; db = nullptr; env.WaitIdle(); }

    descs.emplace_back("beta", base);
    descs.emplace_back("gamma", base);
    handles.clear();
    st = leveldb::DB::Open(plain, path, descs, &handles, &db);
    env.WaitIdle();
    check("open-unknown-family", st.IsInvalidArgument(), st.ToString());
    if (st.ok()) { delete db; db = nullptr; env.WaitIdle(); }

    leveldb::Options creating = plain;
    creating.create_missing_column_families = true;
    handles.clear();
    st = leveldb::DB::Open(creating, path, descs, &handles, &db);
    env.WaitIdle();
    bool reopened = st.ok() && handles.size() == 4;
    check("open-creating-missing-family",
          reopened && handles[1]->GetName() == "alpha" &&
              handles[1]->GetID() == alpha_id && get_cf(handles[1]) == "later",
          st.ToString() + (reopened ? " k=" + get_cf(handles[1]) : ""));
    if (!reopened) {
      check("drop-family", false, "could not reopen");
      if (db != nullptr) delete db;
      DestroyScenarioDir(path);
      return *passed == *total;
    }

    // dropping: the handle stops working, the name comes back free, and the
    // default family cannot be dropped
    st = db->DropColumnFamily(handles[3]);  // gamma
    env.WaitIdle();
    leveldb::Status after = db->Put(leveldb::WriteOptions(), handles[3], "x", "y");
    std::string v;
    leveldb::Status read_after = db->Get(leveldb::ReadOptions(), handles[3], "x", &v);
    leveldb::Status def_drop = db->DropColumnFamily(handles[0]);
    env.WaitIdle();
    check("drop-family",
          st.ok() && after.IsInvalidArgument() && read_after.IsInvalidArgument() &&
              def_drop.IsInvalidArgument(),
          st.ToString() + "/" + after.ToString() + "/" + read_after.ToString() +
              "/" + def_drop.ToString());
    const uint32_t gamma_id = handles[3]->GetID();
    db->DestroyColumnFamilyHandle(handles[3]);
    env.WaitIdle();

    leveldb::ColumnFamilyHandle* again = nullptr;
    st = db->CreateColumnFamily(base, "gamma", &again);
    env.WaitIdle();
    bool fresh = st.ok() && again != nullptr;
    if (fresh) {
      std::string got;
      leveldb::Status g = db->Get(leveldb::ReadOptions(), again, "x", &got);
      env.WaitIdle();
      fresh = g.IsNotFound() && again->GetID() != gamma_id;
      db->DestroyColumnFamilyHandle(again);
      env.WaitIdle();
    }
    check("recreate-dropped-family", fresh, st.ToString());

    for (size_t i = 1; i < 3; i++) db->DestroyColumnFamilyHandle(handles[i]);
    delete db;
    env.WaitIdle();
    db = nullptr;
    names.clear();
    leveldb::DB::ListColumnFamilies(base, path, &names);
    std::sort(names.begin(), names.end());
    check("families-after-reopen",
          names.size() == 4 && names[0] == "alpha" && names[1] == "beta" &&
              names[2] == "default" && names[3] == "gamma",
          "[" + Join(names) + "]");
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

uint64_t LogBytes(const std::string& dir) {
  uint64_t total = 0;
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return 0;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    std::string n = e->d_name;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0) {
      struct stat st;
      if (stat((dir + "/" + n).c_str(), &st) == 0) total += st.st_size;
    }
  }
  closedir(d);
  return total;
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
    bool ok = CheckGets(&d, leveldb::ReadOptions(), 0, model, &rng, keys, n, where);
    if (!ok) *error = d.error;
    return ok;
  };
  auto count_scan = [&](const char* where) -> bool {
    bool ok = CheckScan(&d, leveldb::ReadOptions(), 0, model, where);
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
        ok = CheckWalk(&d, leveldb::ReadOptions(), 0, model, rng.Below(keys), 20, &rng);
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
  } else if (which == "cfwal") {
    // Four families share the log.  Two of them are written once and then
    // left alone; the third takes 60 MB in small batches.  The engine has
    // to flush the quiet families so the old log files can go.
    const uint64_t quiet_keys = static_cast<uint64_t>(2000 * scale);
    const uint64_t busy_keys = static_cast<uint64_t>(60000 * scale);
    std::vector<leveldb::ColumnFamilyHandle*> handles;
    for (const char* name : {"a", "b", "c"}) {
      leveldb::ColumnFamilyHandle* h = nullptr;
      leveldb::Options o;
      o.env = &d.env;
      o.write_buffer_size = 1 << 20;
      o.max_file_size = d.shape.max_file;
      o.block_size = d.shape.block_size;
      o.compression = leveldb::kNoCompression;
      o.merge_operator = &d.merge_op;
      leveldb::Status st = d.db->CreateColumnFamily(o, name, &h);
      d.env.WaitIdle();
      if (!st.ok()) { *error = "CreateColumnFamily: " + st.ToString(); ok = false; break; }
      handles.push_back(h);
    }
    ConfModel quiet[2];
    if (ok) {
      Phase w;
      for (int q = 0; q < 2 && ok; q++) {
        for (uint64_t id = 0; id < quiet_keys && ok; id++) {
          std::string v = ConfValue(id, 1, 900);
          leveldb::Status st =
              d.db->Put(wo, handles[q + 1], KeyName(id), v);  // families b and c
          if (!st.ok()) { *error = "Put: " + st.ToString(); ok = false; }
          quiet[q][id] = v;
        }
      }
      d.env.WaitIdle();
      // now only family a is written
      for (uint64_t i = 0; i < busy_keys && ok; i++) {
        leveldb::WriteBatch batch;
        for (int j = 0; j < 4; j++) {
          uint64_t id = (i * 4 + j) % (busy_keys);
          std::string v = ConfValue(id, 1 + i, 200);
          batch.Put(handles[0], KeyName(id), v);
          model[id] = v;
        }
        leveldb::Status st = d.db->Write(wo, &batch);
        if (!st.ok()) { *error = "Write: " + st.ToString(); ok = false; }
        if ((i % 4096) == 0) d.env.WaitIdle();
      }
      d.env.WaitIdle();
      w.Report("W_WCHAR", "W_RCHAR");
      Metric("W_LOG_BYTES", LogBytes(dir));
    }
    // every family still reads back correctly, before and after a reopen
    for (int pass = 0; pass < 2 && ok; pass++) {
      for (int q = 0; q < 2 && ok; q++) {
        std::unique_ptr<leveldb::Iterator> it(
            d.db->NewIterator(leveldb::ReadOptions(), handles[q + 1]));
        auto e = quiet[q].begin();
        for (it->SeekToFirst(); it->Valid(); it->Next(), ++e) {
          if (e == quiet[q].end() || it->key().ToString() != KeyName(e->first) ||
              it->value().ToString() != e->second) {
            *error = "family " + std::string(q == 0 ? "b" : "c") +
                     " lost data at " + it->key().ToString();
            ok = false;
            break;
          }
        }
        if (ok && e != quiet[q].end()) {
          *error = "family " + std::string(q == 0 ? "b" : "c") + " is missing keys";
          ok = false;
        }
        it.reset();
        d.env.WaitIdle();
      }
      if (ok) {
        std::unique_ptr<leveldb::Iterator> it(
            d.db->NewIterator(leveldb::ReadOptions(), handles[0]));
        auto e = model.begin();
        for (it->SeekToFirst(); it->Valid(); it->Next(), ++e) {
          if (e == model.end() || it->key().ToString() != KeyName(e->first) ||
              it->value().ToString() != e->second) {
            *error = "family a lost data at " + it->key().ToString();
            ok = false;
            break;
          }
        }
        if (ok && e != model.end()) { *error = "family a is missing keys"; ok = false; }
        it.reset();
        d.env.WaitIdle();
      }
      if (ok && pass == 0) {
        // reopen with every family
        for (leveldb::ColumnFamilyHandle* h : handles) {
          d.db->DestroyColumnFamilyHandle(h);
        }
        handles.clear();
        delete d.db;
        d.db = nullptr;
        d.env.WaitIdle();
        std::vector<leveldb::ColumnFamilyDescriptor> descs;
        leveldb::Options o;
        o.env = &d.env;
        o.write_buffer_size = 1 << 20;
        o.max_file_size = d.shape.max_file;
        o.block_size = d.shape.block_size;
        o.compression = leveldb::kNoCompression;
        o.merge_operator = &d.merge_op;
        descs.emplace_back("default", o);
        for (const char* name : {"a", "b", "c"}) descs.emplace_back(name, o);
        std::vector<leveldb::ColumnFamilyHandle*> got;
        leveldb::Status st = leveldb::DB::Open(o, dir, descs, &got, &d.db);
        d.env.WaitIdle();
        if (!st.ok()) { *error = "reopen: " + st.ToString(); ok = false; break; }
        handles.assign(got.begin() + 1, got.end());
      }
    }
    for (leveldb::ColumnFamilyHandle* h : handles) {
      if (d.db != nullptr) d.db->DestroyColumnFamilyHandle(h);
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
