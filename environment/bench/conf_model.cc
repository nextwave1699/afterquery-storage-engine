#include "conf_model.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>

#include "leveldb/db.h"
#include "leveldb/iterator.h"
#include "leveldb/options.h"
#include "leveldb/write_batch.h"

#include "bench_env.h"

namespace lsmbench {

ScenarioGen::ScenarioGen(uint64_t seed, uint64_t ops) : rng_(seed ^ 0xC0DEull), ops_(ops) {
  keys_ = 300 + rng_.Below(2700);
}

bool ScenarioGen::Next(const ConfModel& model, ConfOp* op) {
  *op = ConfOp();
  if (done_ >= ops_) return false;
  done_++;
  uint64_t roll = rng_.Below(100);
  if (roll < 15) {
    op->kind = kOpWrite;
    op->single = true;
    uint64_t id = rng_.Below(keys_);
    if (rng_.Chance(75)) {
      op->writes.push_back({false, id, ConfValue(id, version_++, rng_.Below(160))});
    } else {
      op->writes.push_back({true, id, ""});
    }
  } else if (roll < 37) {
    op->kind = kOpWrite;
    uint64_t n = 1 + rng_.Below(rng_.Chance(10) ? 200 : 12);
    for (uint64_t i = 0; i < n; i++) {
      uint64_t id = rng_.Below(keys_);
      if (rng_.Chance(70)) {
        op->writes.push_back({false, id, ConfValue(id, version_++, rng_.Below(160))});
      } else {
        op->writes.push_back({true, id, ""});
      }
    }
  } else if (roll < 45) {
    // clear a key range in one batch, then write a few keys back into it
    op->kind = kOpWrite;
    uint64_t lo = rng_.Below(keys_);
    uint64_t hi = lo + 1 + rng_.Below(keys_ / 4 + 1);
    for (auto it = model.lower_bound(lo); it != model.end() && it->first < hi; ++it) {
      op->writes.push_back({true, it->first, ""});
    }
    uint64_t back = rng_.Below(4);
    for (uint64_t i = 0; i < back; i++) {
      uint64_t id = lo + rng_.Below(hi - lo);
      op->writes.push_back({false, id, ConfValue(id, version_++, rng_.Below(160))});
    }
    if (op->writes.empty()) op->writes.push_back({true, lo, ""});
  } else if (roll < 58) {
    op->kind = kOpGets;
    op->count = 8 + rng_.Below(24);
  } else if (roll < 72) {
    op->kind = kOpWalk;
    op->id = rng_.Below(keys_ + 20);
    op->count = 1 + rng_.Below(80);
  } else if (roll < 77) {
    op->kind = kOpScan;
  } else if (roll < 86) {
    op->kind = snap_open_ ? kOpSnapCheck : kOpSnapTake;
    snap_open_ = !snap_open_;
  } else if (roll < 91) {
    op->kind = kOpReopen;
    snap_open_ = false;
  } else {
    op->kind = kOpCompact;
    if (rng_.Chance(30)) {
      op->count = 0;
    } else {
      op->id = rng_.Below(keys_);
      op->count = 1 + rng_.Below(keys_ / 3 + 1);
    }
  }
  return true;
}

void ScenarioGen::Apply(const ConfOp& op, ConfModel* model) {
  if (op.kind != kOpWrite) return;
  for (const ConfWrite& w : op.writes) {
    if (w.del) {
      model->erase(w.id);
    } else {
      (*model)[w.id] = w.value;
    }
  }
}

ConfModel ModelAfterWrites(uint64_t seed, uint64_t ops, uint64_t writes, bool* past_end) {
  ScenarioGen gen(seed, ops);
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
  leveldb::Options o;
  leveldb::DestroyDB(dir, o);
  rmdir(dir.c_str());
}

namespace {

struct Db {
  BenchEnv env;
  leveldb::DB* db = nullptr;
  std::string dir;
  std::string error;

  bool Fail(const std::string& what) {
    if (error.empty()) error = what;
    return false;
  }

  bool Open(bool create) {
    leveldb::Options o;
    o.create_if_missing = create;
    o.paranoid_checks = true;
    o.env = &env;
    // Small buffers and files so a few hundred operations already produce
    // flushes, several levels and compactions.
    o.write_buffer_size = 64 * 1024;
    o.max_file_size = 96 * 1024;
    o.block_size = 1024;
    o.compression = leveldb::kNoCompression;
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
    if (!st.ok()) return d->Fail(std::string(where) + ": Get " + KeyName(id) + ": " + st.ToString());
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
      return d->Fail(std::string(where) + ": backwards at " + it->key().ToString() + ", expected " +
                     KeyName(r->first));
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
      return d->Fail("walk from " + KeyName(start) + ": iterator " +
                     (it->Valid() ? "at " + it->key().ToString() : std::string("invalid")) +
                     ", expected " + (valid ? KeyName(e->first) : std::string("the end")));
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

}  // namespace

bool RunScenario(const std::string& dir, uint64_t seed, uint64_t ops, const std::string& crash,
                 int ack_fd, std::string* error) {
  Db d;
  d.dir = dir;
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
  ScenarioGen gen(seed, ops);
  ConfRng check_rng(seed ^ 0x7E57ull);
  ConfModel model, snap_model;
  const leveldb::Snapshot* snap = nullptr;
  uint64_t writes = 0;
  ConfOp op;
  bool ok = true;
  while (ok && gen.Next(model, &op)) {
    leveldb::ReadOptions ro;
    switch (op.kind) {
      case kOpWrite: {
        leveldb::Status st;
        if (op.single && op.writes[0].del) {
          st = d.db->Delete(leveldb::WriteOptions(), KeyName(op.writes[0].id));
        } else if (op.single) {
          st = d.db->Put(leveldb::WriteOptions(), KeyName(op.writes[0].id), op.writes[0].value);
        } else {
          leveldb::WriteBatch batch;
          for (const ConfWrite& w : op.writes) {
            if (w.del) batch.Delete(KeyName(w.id));
            else batch.Put(KeyName(w.id), w.value);
          }
          st = d.db->Write(leveldb::WriteOptions(), &batch);
        }
        d.env.WaitIdle();
        if (!st.ok()) { ok = d.Fail("write: " + st.ToString()); break; }
        ScenarioGen::Apply(op, &model);
        WriteAck(ack_fd, ++writes);
        break;
      }
      case kOpGets:
        ok = CheckGets(&d, ro, model, &check_rng, gen.keys(), op.count, "reads");
        break;
      case kOpWalk:
        ok = CheckWalk(&d, ro, model, op.id, op.count, &check_rng);
        break;
      case kOpScan:
        ok = CheckScan(&d, ro, model, "scan");
        break;
      case kOpSnapTake:
        if (snap != nullptr) d.db->ReleaseSnapshot(snap);
        snap = d.db->GetSnapshot();
        snap_model = model;
        break;
      case kOpSnapCheck:
        if (snap != nullptr) {
          ro.snapshot = snap;
          ok = CheckGets(&d, ro, snap_model, &check_rng, gen.keys(), 24, "snapshot reads") &&
               CheckScan(&d, ro, snap_model, "snapshot scan");
          d.db->ReleaseSnapshot(snap);
          snap = nullptr;
        }
        break;
      case kOpReopen:
        if (snap != nullptr) {
          d.db->ReleaseSnapshot(snap);
          snap = nullptr;
        }
        d.Close();
        ok = d.Open(false) && CheckGets(&d, ro, model, &check_rng, gen.keys(), 32, "after reopen");
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
        ok = CheckGets(&d, ro, model, &check_rng, gen.keys(), 32, "after compaction");
        break;
    }
  }
  if (snap != nullptr) d.db->ReleaseSnapshot(snap);
  if (ok) ok = CheckScan(&d, leveldb::ReadOptions(), model, "final scan");
  if (ok) {
    d.Close();
    ok = d.Open(false) && CheckScan(&d, leveldb::ReadOptions(), model, "final reopen");
  }
  d.Close();
  if (!ok) *error = d.error;
  return ok;
}

bool CheckRecovered(const std::string& dir, uint64_t seed, uint64_t ops, uint64_t acked,
                    uint64_t* recovered, std::string* error) {
  bool end0 = false, end1 = false;
  ConfModel m0 = ModelAfterWrites(seed, ops, acked, &end0);
  ConfModel m1 = ModelAfterWrites(seed, ops, acked + 1, &end1);
  Db d;
  d.dir = dir;
  // With nothing acknowledged the crash may have come before the database
  // existed, so it may be created here.
  if (!d.Open(acked == 0)) {
    *error = d.error;
    return false;
  }
  // Which state is it?  Try the acknowledged one first; a database that
  // matches neither lost acknowledged writes or kept half a batch.
  Db* dp = &d;
  bool is0 = CheckScan(dp, leveldb::ReadOptions(), m0, "recovered");
  std::string err0 = d.error;
  d.error.clear();
  bool is1 = !is0 && !end1 && CheckScan(dp, leveldb::ReadOptions(), m1, "recovered");
  if (!is0 && !is1) {
    *error = "recovered state matches neither " + std::to_string(acked) + " nor " +
             std::to_string(acked + 1) + " writes: " + err0;
    d.Close();
    return false;
  }
  *recovered = is0 ? acked : acked + 1;
  // The database has to be usable afterwards: write, reopen, read back.
  ConfModel& m = is0 ? m0 : m1;
  for (uint64_t i = 0; i < 50; i++) {
    uint64_t id = (seed * 7919 + i * 104729) % 4000;
    std::string v = ConfValue(id, 1000000000ull + i, 40);
    leveldb::Status st = d.db->Put(leveldb::WriteOptions(), KeyName(id), v);
    d.env.WaitIdle();
    if (!st.ok()) {
      *error = "write after recovery: " + st.ToString();
      d.Close();
      return false;
    }
    m[id] = v;
  }
  d.Close();
  bool ok = d.Open(false) && CheckScan(&d, leveldb::ReadOptions(), m, "after recovery and reopen");
  d.Close();
  if (!ok) *error = d.error;
  return ok;
}

}  // namespace lsmbench
