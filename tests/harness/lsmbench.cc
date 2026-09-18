// lsmbench: the storage-engine benchmark and checker.
//
//   lsmbench describe --seed S --scale X
//       print the workload's size (batches, logical bytes) as JSON
//   lsmbench run --db DIR --seed S --scale X [--stats F] [--crash EVENT:N[:after]]
//                [--ack F] [--arm-at-batch N | --arm-at-reopen] [--reopen-at N]
//       execute phases A..E on a fresh database, checking every read against
//       the model; optionally crash (SIGKILL) at an Env event
//   lsmbench read --db DIR --seed S --scale X [--stats F]
//       execute phase F (reads only) on the database `run` produced
//   lsmbench check --db DIR --seed S --scale X --batches B [--continue K]
//                  [--paranoid] [--stats F]
//       verify that the database holds exactly the state after B (or B+1,
//       an in-flight batch) write batches; then optionally execute K more
//       batches and close cleanly.  Prints APPLIED <n> and FINAL_BATCHES <m>.
//   lsmbench selftest --db DIR
//       small functional test of the engine API
//
// Every mode also takes --workload (default mixed; see workload.h).
//
// Exit status: 0 ok, 2 a read returned the wrong result, 3 engine error,
// 4 usage error.  All engine I/O goes through BenchEnv (bench_env.h).
#include <sys/resource.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "leveldb/iterator.h"
#include "leveldb/options.h"
#include "leveldb/write_batch.h"

#include "bench_env.h"
#include "workload.h"

using namespace lsmbench;

namespace {

const int kExitMismatch = 2;
const int kExitEngine = 3;
const int kExitUsage = 4;

struct Args {
  std::string mode;
  std::string db;
  WorkloadKind workload = kMixed;
  uint64_t seed = 1;
  double scale = 1.0;
  std::string stats;
  std::string crash;
  std::string ack;
  uint64_t arm_at_batch = 0;
  bool arm_at_reopen = false;
  uint64_t reopen_at = 0;
  uint64_t batches = 0;
  uint64_t cont = 0;
  bool paranoid = false;
  std::string info_log;
  bool quiet = false;
  bool stop_at_end = false;
  bool reuse_logs = false;   // fixture generation: Options::reuse_logs
  bool plain_tables = false; // fixture generation: no filter, 16K blocks, restart 64
};

void Usage() {
  fprintf(stderr,
          "usage: lsmbench (describe|run|read|check|selftest) --db DIR --seed S --scale X [--workload W] ...\n");
  exit(kExitUsage);
}

Args ParseArgs(int argc, char** argv) {
  Args a;
  if (argc < 2) Usage();
  a.mode = argv[1];
  for (int i = 2; i < argc; i++) {
    std::string k = argv[i];
    auto need = [&](void) -> std::string {
      if (i + 1 >= argc) Usage();
      return argv[++i];
    };
    if (k == "--db") a.db = need();
    else if (k == "--seed") a.seed = strtoull(need().c_str(), nullptr, 10);
    else if (k == "--scale") a.scale = strtod(need().c_str(), nullptr);
    else if (k == "--stats") a.stats = need();
    else if (k == "--crash") a.crash = need();
    else if (k == "--ack") a.ack = need();
    else if (k == "--arm-at-batch") a.arm_at_batch = strtoull(need().c_str(), nullptr, 10);
    else if (k == "--arm-at-reopen") a.arm_at_reopen = true;
    else if (k == "--reopen-at") a.reopen_at = strtoull(need().c_str(), nullptr, 10);
    else if (k == "--batches") a.batches = strtoull(need().c_str(), nullptr, 10);
    else if (k == "--continue") a.cont = strtoull(need().c_str(), nullptr, 10);
    else if (k == "--paranoid") a.paranoid = true;
    else if (k == "--info-log") a.info_log = need();
    else if (k == "--quiet") a.quiet = true;
    else if (k == "--stop-at-end") a.stop_at_end = true;
    else if (k == "--reuse-logs") a.reuse_logs = true;
    else if (k == "--plain-tables") a.plain_tables = true;
    else if (k == "--workload") { if (!ParseWorkloadKind(need(), &a.workload)) Usage(); }
    else Usage();
  }
  if (a.scale <= 0 || a.scale > 8) Usage();
  return a;
}

// Same options for both engines. Small buffers and files so the workload
// reaches several levels.
struct DbResources {
  std::unique_ptr<leveldb::Cache> cache;
  std::unique_ptr<const leveldb::FilterPolicy> filter;
  std::unique_ptr<leveldb::Logger> logger;
};

leveldb::Options MakeOptions(BenchEnv* env, DbResources* res, bool create,
                             bool paranoid, const std::string& info_log,
                             bool reuse_logs = false, bool plain_tables = false) {
  leveldb::Options o;
  o.env = env;
  o.create_if_missing = create;
  o.error_if_exists = false;
  o.paranoid_checks = paranoid;
  o.write_buffer_size = 4 * 1024 * 1024;
  o.max_file_size = 2 * 1024 * 1024;
  o.max_open_files = 500;
  o.block_size = 4096;
  o.block_restart_interval = 16;
  o.compression = leveldb::kNoCompression;
  o.reuse_logs = reuse_logs;
  res->cache.reset(leveldb::NewLRUCache(8 * 1024 * 1024));
  o.block_cache = res->cache.get();
  if (plain_tables) {
    o.block_size = 16384;
    o.block_restart_interval = 64;
    o.filter_policy = nullptr;
  } else {
    res->filter.reset(leveldb::NewBloomFilterPolicy(10));
    o.filter_policy = res->filter.get();
  }
  FILE* f = info_log.empty() ? nullptr : fopen(info_log.c_str(), "a");
  res->logger.reset(new BenchLogger(f));
  o.info_log = res->logger.get();
  return o;
}

class Runner {
 public:
  Runner(const Args& a, BenchEnv* env)
      : args_(a), env_(env), wl_(a.seed, a.scale, a.workload),
        model_(a.seed, wl_.shape().universe), db_(nullptr), snapshot_(nullptr) {
    ack_fd_ = -1;
    if (!a.ack.empty()) {
      ack_fd_ = ::open(a.ack.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
      if (ack_fd_ < 0) Die(kExitUsage, "cannot open ack file");
      WriteAck(0);
    }
  }
  ~Runner() { CloseDb(); }

  Workload& workload() { return wl_; }
  Model& model() { return model_; }
  uint64_t executed_batches() const { return executed_batches_; }
  uint64_t gets() const { return gets_; }
  uint64_t scans() const { return scans_; }
  uint64_t scanned_entries() const { return scanned_entries_; }

  void OpenDb(bool create) {
    leveldb::Options o = MakeOptions(env_, &res_, create, args_.paranoid, args_.info_log,
                                     args_.reuse_logs, args_.plain_tables);
    leveldb::Status s = leveldb::DB::Open(o, args_.db, &db_);
    if (!s.ok()) Die(kExitEngine, "DB::Open: " + s.ToString());
    env_->WaitIdle();
    opens_++;
  }

  void CloseDb() {
    if (snapshot_ != nullptr && db_ != nullptr) {
      db_->ReleaseSnapshot(snapshot_);
      snapshot_ = nullptr;
    }
    if (db_ != nullptr) {
      env_->WaitIdle();
      delete db_;
      db_ = nullptr;
      env_->WaitIdle();
    }
  }

  void Reopen() {
    CloseDb();
    if (args_.arm_at_reopen) env_->crash()->Arm();
    OpenDb(false);
  }

  // Apply a write batch to the model only (replay).
  void ReplayBatch(const Step& st) {
    for (const Op& op : st.ops) {
      if (op.del) model_.Del(op.id); else model_.Put(op.id);
    }
  }

  // Execute one step against the database and check it.
  void Execute(const Step& st) {
    switch (st.kind) {
      case kStepPhase:
        if (!args_.quiet) {
          fprintf(stderr, "phase %c  batches=%llu wseq=%u logical=%llu MB written=%llu MB read=%llu MB\n",
                  st.phase, static_cast<unsigned long long>(executed_batches_), model_.wseq,
                  static_cast<unsigned long long>(model_.logical_bytes >> 20),
                  static_cast<unsigned long long>(env_->counters()->total_written() >> 20),
                  static_cast<unsigned long long>(env_->counters()->total_read() >> 20));
        }
        break;
      case kStepBatch: DoBatch(st); break;
      case kStepGet: DoGet(st.id, nullptr, 0); break;
      case kStepScan: DoScan(st.id, st.count); break;
      case kStepRScan: DoRScan(st.id, st.count); break;
      case kStepSnapTake: DoSnapTake(st); break;
      case kStepSnapCheck: DoSnapCheck(); break;
      case kStepFullScan: DoFullScan(); break;
    }
  }

  // Checks the whole database against the model. One in-flight batch is
  // allowed to be either applied or not (the two maps hold its ids' versions
  // before and after it). Returns true if it was applied.
  bool Verify(const std::map<uint64_t, uint32_t>& inflight_before,
              const std::map<uint64_t, uint32_t>& inflight_after) {
    // Forward scan first; this also decides whether the in-flight batch landed.
    int inflight_state = -1;  // -1 unknown, 0 old, 1 new
    auto decide = [&](uint64_t id, const leveldb::Slice* value, bool found) {
      auto ib = inflight_before.find(id);
      if (ib == inflight_before.end()) return false;
      uint32_t old_v = ib->second;
      uint32_t new_v = inflight_after.at(id);
      bool old_ok = Matches(id, old_v, value, found);
      bool new_ok = Matches(id, new_v, value, found);
      if (old_ok && new_ok) return true;  // both the same, no information
      int state = old_ok ? 0 : (new_ok ? 1 : -2);
      if (state == -2) Mismatch(id, "in-flight key matches neither the state before nor after the batch");
      if (inflight_state == -1) inflight_state = state;
      else if (inflight_state != state) Mismatch(id, "in-flight batch applied only partially");
      return true;
    };
    leveldb::ReadOptions ro;
    ro.verify_checksums = true;
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(ro));
    it->SeekToFirst();
    uint64_t expect = 0;
    uint64_t seen = 0;
    while (true) {
      if (!it->Valid()) {
        if (!it->status().ok()) Die(kExitEngine, "iterator: " + it->status().ToString());
        // Nothing left in the db, so the rest of the ids must be dead.
        while (expect < model_.ver.size()) {
          if (inflight_before.count(expect)) {
            decide(expect, nullptr, false);
          } else if (model_.Live(expect)) {
            Mismatch(expect, "missing from full scan at end");
          }
          expect++;
        }
        break;
      }
      uint64_t id;
      leveldb::Slice k = it->key();
      if (!IdOf(k.data(), k.size(), &id)) Die(kExitMismatch, "malformed key in database: " + k.ToString());
      if (id >= model_.ver.size()) Mismatch(id, "key outside the universe");
      if (id < expect) Mismatch(id, "key out of order or duplicated in full scan");
      // ids in [expect, id) must be absent
      while (expect < id) {
        if (inflight_before.count(expect)) {
          decide(expect, nullptr, false);
        } else if (model_.Live(expect)) {
          Mismatch(expect, "missing from full scan");
        }
        expect++;
      }
      leveldb::Slice v = it->value();
      if (inflight_before.count(id)) {
        decide(id, &v, true);
      } else {
        if (!model_.Live(id)) Mismatch(id, "deleted or never-written key present");
        if (!ValueGen::Equals(model_.seed, id, model_.ver[id], v.data(), v.size())) {
          Mismatch(id, "wrong value in full scan");
        }
      }
      seen++;
      expect = id + 1;
      it->Next();
    }
    if (!it->status().ok()) Die(kExitEngine, "iterator: " + it->status().ToString());
    bool applied = (inflight_state == 1);
    if (applied) {
      for (auto& kv : inflight_after) model_.ver[kv.first] = kv.second;
    }
    // Point reads of a sample (present, deleted, never written), through Get.
    Rng r(args_.seed ^ 0xC4EC ^ args_.batches);
    uint64_t n = wl_.shape().f_gets / 2 + 500;
    for (uint64_t i = 0; i < n; i++) {
      uint64_t id = r.Chance(85) ? r.Below(wl_.shape().universe) : r.Below(wl_.shape().read_universe);
      DoGet(id, nullptr, 0);
    }
    // Reverse scan of everything.
    it.reset(db_->NewIterator(ro));
    it->SeekToLast();
    uint64_t exp = model_.PrevLive(model_.ver.size() - 1);
    while (it->Valid()) {
      uint64_t id;
      leveldb::Slice k = it->key();
      if (!IdOf(k.data(), k.size(), &id)) Die(kExitMismatch, "malformed key: " + k.ToString());
      if (exp == UINT64_MAX || id != exp) Mismatch(id, "reverse scan order mismatch");
      leveldb::Slice v = it->value();
      if (!ValueGen::Equals(model_.seed, id, model_.ver[id], v.data(), v.size())) {
        Mismatch(id, "wrong value in reverse scan");
      }
      exp = (exp == 0) ? UINT64_MAX : model_.PrevLive(exp - 1);
      it->Prev();
    }
    if (!it->status().ok()) Die(kExitEngine, "iterator: " + it->status().ToString());
    if (exp != UINT64_MAX) Mismatch(exp, "reverse scan ended early");
    fprintf(stderr, "verify: %llu live keys, wseq=%u, in-flight batch %s\n",
            static_cast<unsigned long long>(seen), model_.wseq,
            inflight_before.empty() ? "n/a" : (applied ? "applied" : "not applied"));
    return applied;
  }

  uint64_t opens() const { return opens_; }

 private:
  bool Matches(uint64_t id, uint32_t ver, const leveldb::Slice* value, bool found) {
    bool live = ver != 0 && (ver & kDeletedBit) == 0;
    if (!live) return !found;
    if (!found) return false;
    return ValueGen::Equals(model_.seed, id, ver, value->data(), value->size());
  }

  void Die(int code, const std::string& msg) {
    fprintf(stderr, "lsmbench: %s\n", msg.c_str());
    fflush(stderr);
    _exit(code);
  }
  void Mismatch(uint64_t id, const char* what) {
    fprintf(stderr, "lsmbench: MISMATCH id=%llu (%s): %s\n",
            static_cast<unsigned long long>(id), KeyOf(id).c_str(), what);
    fflush(stderr);
    _exit(kExitMismatch);
  }

  void WriteAck(uint64_t n) {
    if (ack_fd_ < 0) return;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%llu\n", static_cast<unsigned long long>(n));
    if (::pwrite(ack_fd_, buf, len, 0) != len) Die(kExitUsage, "ack write failed");
    if (::ftruncate(ack_fd_, len) != 0) Die(kExitUsage, "ack truncate failed");
  }

  void DoBatch(const Step& st) {
    env_->crash()->Before(kEvOp);
    leveldb::WriteBatch wb;
    uint32_t w = model_.wseq;
    for (const Op& op : st.ops) {
      w++;
      if (op.del) {
        wb.Delete(KeyOf(op.id));
      } else {
        wb.Put(KeyOf(op.id), ValueGen::Make(model_.seed, op.id, w));
      }
    }
    leveldb::WriteOptions wo;
    wo.sync = false;
    leveldb::Status s = db_->Write(wo, &wb);
    if (!s.ok()) Die(kExitEngine, "Write: " + s.ToString());
    ReplayBatch(st);
    executed_batches_++;
    env_->WaitIdle();
    WriteAck(executed_batches_);
    if (args_.arm_at_batch != 0 && executed_batches_ == args_.arm_at_batch) env_->crash()->Arm();
    if (args_.reopen_at != 0 && executed_batches_ == args_.reopen_at) Reopen();
  }

  // Get `id` and compare with version `ver` (0 => use the model).
  void DoGet(uint64_t id, const leveldb::Snapshot* snap, uint32_t ver) {
    leveldb::ReadOptions ro;
    ro.snapshot = snap;
    ro.verify_checksums = args_.paranoid;
    std::string v;
    leveldb::Status s = db_->Get(ro, KeyOf(id), &v);
    gets_++;
    if (snap == nullptr) ver = (id < model_.ver.size()) ? model_.ver[id] : 0;
    bool live = ver != 0 && (ver & kDeletedBit) == 0;
    if (s.IsNotFound()) {
      if (live) Mismatch(id, "Get: NotFound for a live key");
    } else if (!s.ok()) {
      Die(kExitEngine, "Get: " + s.ToString());
    } else {
      if (!live) Mismatch(id, "Get: value for a deleted/never-written key");
      if (!ValueGen::Equals(model_.seed, id, ver, v.data(), v.size())) Mismatch(id, "Get: wrong value");
    }
    env_->WaitIdle();
  }

  void DoScan(uint64_t start, uint32_t count) {
    leveldb::ReadOptions ro;
    ro.verify_checksums = args_.paranoid;
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(ro));
    it->Seek(KeyOf(start));
    uint64_t expect = model_.NextLive(start);
    for (uint32_t i = 0; i < count; i++) {
      if (!it->Valid()) {
        if (!it->status().ok()) Die(kExitEngine, "iterator: " + it->status().ToString());
        if (expect < model_.ver.size()) Mismatch(expect, "scan ended early");
        break;
      }
      uint64_t id;
      leveldb::Slice k = it->key();
      if (!IdOf(k.data(), k.size(), &id)) Die(kExitMismatch, "malformed key: " + k.ToString());
      if (expect >= model_.ver.size()) Mismatch(id, "scan returned a key past the end");
      if (id != expect) Mismatch(id, "scan order mismatch");
      leveldb::Slice v = it->value();
      if (!ValueGen::Equals(model_.seed, id, model_.ver[id], v.data(), v.size())) Mismatch(id, "scan: wrong value");
      scanned_entries_++;
      expect = model_.NextLive(id + 1);
      it->Next();
    }
    scans_++;
    it.reset();
    env_->WaitIdle();
  }

  void DoRScan(uint64_t start, uint32_t count) {
    leveldb::ReadOptions ro;
    ro.verify_checksums = args_.paranoid;
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(ro));
    it->Seek(KeyOf(start));
    if (!it->Valid()) {
      it->SeekToLast();
    } else if (it->key().ToString() != KeyOf(start)) {
      it->Prev();
    }
    uint64_t expect = model_.PrevLive(start);
    for (uint32_t i = 0; i < count; i++) {
      if (!it->Valid()) {
        if (!it->status().ok()) Die(kExitEngine, "iterator: " + it->status().ToString());
        if (expect != UINT64_MAX) Mismatch(expect, "reverse scan ended early");
        break;
      }
      uint64_t id;
      leveldb::Slice k = it->key();
      if (!IdOf(k.data(), k.size(), &id)) Die(kExitMismatch, "malformed key: " + k.ToString());
      if (expect == UINT64_MAX || id != expect) Mismatch(id, "reverse scan order mismatch");
      leveldb::Slice v = it->value();
      if (!ValueGen::Equals(model_.seed, id, model_.ver[id], v.data(), v.size())) Mismatch(id, "rscan: wrong value");
      scanned_entries_++;
      expect = (expect == 0) ? UINT64_MAX : model_.PrevLive(expect - 1);
      it->Prev();
    }
    scans_++;
    it.reset();
    env_->WaitIdle();
  }

  void DoSnapTake(const Step& st) {
    if (snapshot_ != nullptr) db_->ReleaseSnapshot(snapshot_);
    snapshot_ = db_->GetSnapshot();
    snap_sample_.clear();
    for (uint64_t id : st.ids) snap_sample_.emplace_back(id, model_.ver[id]);
  }

  void DoSnapCheck() {
    if (snapshot_ == nullptr) return;
    for (auto& p : snap_sample_) DoGet(p.first, snapshot_, p.second);
    db_->ReleaseSnapshot(snapshot_);
    snapshot_ = nullptr;
    env_->WaitIdle();
  }

  void DoFullScan() {
    std::map<uint64_t, uint32_t> none;
    Verify(none, none);
  }

  const Args& args_;
  BenchEnv* env_;
  Workload wl_;
  Model model_;
  DbResources res_;
  leveldb::DB* db_;
  const leveldb::Snapshot* snapshot_;
  std::vector<std::pair<uint64_t, uint32_t>> snap_sample_;
  int ack_fd_;
  uint64_t executed_batches_ = 0;
  uint64_t gets_ = 0, scans_ = 0, scanned_entries_ = 0, opens_ = 0;
};

void WriteStats(const Args& a, BenchEnv* env, Runner* r, uint64_t elapsed_ms,
                const char* extra_key, uint64_t extra_val) {
  if (a.stats.empty()) return;
  const Counters* c = env->counters();
  static const char* kinds[] = {"log", "table", "manifest", "other"};
  std::string s = "{\n";
  char buf[512];
  snprintf(buf, sizeof(buf),
           "  \"mode\": \"%s\", \"seed\": %llu, \"scale\": %.6f,\n"
           "  \"batches\": %llu, \"logical_bytes\": %llu, \"puts\": %llu, \"dels\": %llu,\n"
           "  \"gets\": %llu, \"scans\": %llu, \"scanned_entries\": %llu, \"opens\": %llu,\n"
           "  \"jobs\": %llu, \"elapsed_ms\": %llu, \"%s\": %llu,\n",
           a.mode.c_str(), static_cast<unsigned long long>(a.seed), a.scale,
           static_cast<unsigned long long>(r->executed_batches()),
           static_cast<unsigned long long>(r->model().logical_bytes),
           static_cast<unsigned long long>(r->model().puts),
           static_cast<unsigned long long>(r->model().dels),
           static_cast<unsigned long long>(r->gets()),
           static_cast<unsigned long long>(r->scans()),
           static_cast<unsigned long long>(r->scanned_entries()),
           static_cast<unsigned long long>(r->opens()),
           static_cast<unsigned long long>(env->jobs_run()),
           static_cast<unsigned long long>(elapsed_ms), extra_key,
           static_cast<unsigned long long>(extra_val));
  s += buf;
  auto arr = [&](const char* name, const std::atomic<uint64_t>* v) {
    snprintf(buf, sizeof(buf), "  \"%s\": {\"%s\": %llu, \"%s\": %llu, \"%s\": %llu, \"%s\": %llu},\n",
             name, kinds[0], static_cast<unsigned long long>(v[0].load()), kinds[1],
             static_cast<unsigned long long>(v[1].load()), kinds[2],
             static_cast<unsigned long long>(v[2].load()), kinds[3],
             static_cast<unsigned long long>(v[3].load()));
    s += buf;
  };
  arr("written", c->written);
  arr("read", c->read);
  arr("created", c->created);
  arr("removed", c->removed);
  arr("syncs", c->syncs);
  snprintf(buf, sizeof(buf), "  \"renames\": %llu, \"total_written\": %llu, \"total_read\": %llu\n}\n",
           static_cast<unsigned long long>(c->renames.load()),
           static_cast<unsigned long long>(c->total_written()),
           static_cast<unsigned long long>(c->total_read()));
  s += buf;
  std::string tmp = a.stats + ".tmp";
  FILE* f = fopen(tmp.c_str(), "w");
  if (f == nullptr) {
    fprintf(stderr, "lsmbench: cannot write stats %s\n", a.stats.c_str());
    return;
  }
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
  rename(tmp.c_str(), a.stats.c_str());
}

uint64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

int Describe(const Args& a) {
  Workload wl(a.seed, a.scale, a.workload);
  Model m(a.seed, wl.shape().universe);
  Step st;
  uint64_t steps = 0, gets = 0, scans = 0;
  while (wl.Next(&st)) {
    steps++;
    if (st.kind == kStepBatch) {
      for (const Op& op : st.ops) {
        if (op.del) m.Del(op.id); else m.Put(op.id);
      }
    } else if (st.kind == kStepGet) gets++;
    else if (st.kind == kStepScan) scans++;
  }
  uint64_t live = 0, live_bytes = 0;
  for (uint64_t id = 0; id < m.ver.size(); id++) {
    if (m.Live(id)) {
      live++;
      live_bytes += kKeyLen + ValueGen::Length(a.seed, id, m.ver[id]);
    }
  }
  std::vector<Step> f;
  wl.PhaseF(&f);
  printf("{\"seed\": %llu, \"scale\": %.6f, \"batches\": %llu, \"steps\": %llu, "
         "\"logical_bytes\": %llu, \"puts\": %llu, \"dels\": %llu, \"gets\": %llu, "
         "\"scans\": %llu, \"universe\": %llu, \"live_keys\": %llu, \"live_bytes\": %llu, "
         "\"phase_f_steps\": %llu}\n",
         static_cast<unsigned long long>(a.seed), a.scale,
         static_cast<unsigned long long>(wl.batches()), static_cast<unsigned long long>(steps),
         static_cast<unsigned long long>(m.logical_bytes), static_cast<unsigned long long>(m.puts),
         static_cast<unsigned long long>(m.dels), static_cast<unsigned long long>(gets),
         static_cast<unsigned long long>(scans), static_cast<unsigned long long>(m.ver.size()),
         static_cast<unsigned long long>(live), static_cast<unsigned long long>(live_bytes),
         static_cast<unsigned long long>(f.size()));
  return 0;
}

int Run(const Args& a) {
  BenchEnv env;
  if (!env.crash()->Configure(a.crash)) {
    fprintf(stderr, "lsmbench: bad --crash spec\n");
    return kExitUsage;
  }
  if (a.arm_at_batch != 0 || a.arm_at_reopen) env.crash()->Disarm();
  uint64_t t0 = NowMs();
  Runner r(a, &env);
  r.OpenDb(true);
  Step st;
  while (r.workload().Next(&st)) r.Execute(st);
  r.CloseDb();
  WriteStats(a, &env, &r, NowMs() - t0, "crash_events_seen", env.crash()->count());
  fprintf(stderr, "run done: batches=%llu logical=%llu written=%llu read=%llu\n",
          static_cast<unsigned long long>(r.executed_batches()),
          static_cast<unsigned long long>(r.model().logical_bytes),
          static_cast<unsigned long long>(env.counters()->total_written()),
          static_cast<unsigned long long>(env.counters()->total_read()));
  return 0;
}

int Read(const Args& a) {
  BenchEnv env;
  uint64_t t0 = NowMs();
  Runner r(a, &env);
  // Rebuild the model: replay every write batch of phases A..E.
  Step st;
  while (r.workload().Next(&st)) {
    if (st.kind == kStepBatch) r.ReplayBatch(st);
  }
  r.OpenDb(false);
  std::vector<Step> f;
  r.workload().PhaseF(&f);
  for (const Step& s : f) r.Execute(s);
  r.CloseDb();
  WriteStats(a, &env, &r, NowMs() - t0, "phase_f_steps", f.size());
  fprintf(stderr, "read done: gets=%llu scans=%llu read=%llu written=%llu\n",
          static_cast<unsigned long long>(r.gets()), static_cast<unsigned long long>(r.scans()),
          static_cast<unsigned long long>(env.counters()->total_read()),
          static_cast<unsigned long long>(env.counters()->total_written()));
  return 0;
}

int Check(const Args& a) {
  BenchEnv env;
  uint64_t t0 = NowMs();
  Runner r(a, &env);
  r.workload().AllowExtension();  // batches beyond phase E come from phase X
  // Replay the first `batches` write batches into the model, and keep the
  // next batch (possibly in flight at a crash) aside.
  Step st;
  uint64_t applied = 0;
  bool have_next = false;
  Step next;
  while (applied < a.batches && r.workload().Next(&st)) {
    if (st.kind == kStepBatch) {
      r.ReplayBatch(st);
      applied++;
    }
  }
  if (applied < a.batches) {
    fprintf(stderr, "lsmbench: workload has only %llu batches\n",
            static_cast<unsigned long long>(applied));
    return kExitUsage;
  }
  std::map<uint64_t, uint32_t> before, after;
  std::vector<Step> skipped;  // non-batch steps between batch B and B+1
  while (r.workload().Next(&st)) {
    if (st.kind == kStepBatch) {
      next = st;
      have_next = true;
      break;
    }
    skipped.push_back(st);
  }
  if (have_next) {
    Model& m = r.model();
    for (const Op& op : next.ops) {
      if (!before.count(op.id)) before[op.id] = m.ver[op.id];
    }
    uint32_t w = m.wseq;
    for (const Op& op : next.ops) {
      w++;
      after[op.id] = op.del ? (w | kDeletedBit) : w;
    }
  }
  r.OpenDb(false);
  bool next_applied = r.Verify(before, after);
  uint64_t state = a.batches + (next_applied ? 1 : 0);
  printf("APPLIED %llu\n", static_cast<unsigned long long>(state));
  fflush(stdout);
  uint64_t final_batches = state;
  if (a.cont > 0) {
    if (next_applied) {
      // Verify() only patched ver[] for the in-flight ids; replay the batch
      // properly so wseq and the byte counters advance too.
      Model& m = r.model();
      for (auto& kv : before) m.ver[kv.first] = kv.second;
      r.ReplayBatch(next);
    }
    uint64_t done = 0;
    if (have_next && !next_applied) {
      r.Execute(next);
      done++;
    }
    while (done < a.cont && r.workload().Next(&st)) {
      r.Execute(st);
      if (st.kind == kStepBatch) done++;
    }
    final_batches = state + done;
    r.CloseDb();
    printf("FINAL_BATCHES %llu\n", static_cast<unsigned long long>(final_batches));
    fflush(stdout);
  } else {
    r.CloseDb();
  }
  WriteStats(a, &env, &r, NowMs() - t0, "state_batches", final_batches);
  return 0;
}

// A small functional test through the public API.
int SelfTest(const Args& a) {
  BenchEnv env;
  DbResources res;
  leveldb::Options o = MakeOptions(&env, &res, true, true, "");
  leveldb::DB* db = nullptr;
  leveldb::Status s = leveldb::DB::Open(o, a.db, &db);
  auto fail = [&](const std::string& what) {
    fprintf(stderr, "selftest: %s\n", what.c_str());
    delete db;
    _exit(kExitMismatch);
  };
  if (!s.ok()) fail("open: " + s.ToString());
  leveldb::WriteOptions wo;
  leveldb::ReadOptions ro;
  ro.verify_checksums = true;
  const int n = 20000;
  for (int i = 0; i < n; i++) {
    std::string v(100 + (i % 200), static_cast<char>('a' + i % 26));
    s = db->Put(wo, KeyOf(i), v);
    if (!s.ok()) fail("put: " + s.ToString());
    env.WaitIdle();
  }
  std::string v;
  for (int i = 0; i < n; i += 7) {
    s = db->Get(ro, KeyOf(i), &v);
    if (!s.ok() || v != std::string(100 + (i % 200), static_cast<char>('a' + i % 26))) fail("get mismatch");
  }
  const leveldb::Snapshot* snap = db->GetSnapshot();
  {
    leveldb::WriteBatch wb;
    for (int i = 0; i < n; i += 2) wb.Delete(KeyOf(i));
    for (int i = 1; i < n; i += 2) wb.Put(KeyOf(i), "updated");
    s = db->Write(wo, &wb);
    if (!s.ok()) fail("batch: " + s.ToString());
    env.WaitIdle();
  }
  ro.snapshot = snap;
  s = db->Get(ro, KeyOf(0), &v);
  if (!s.ok() || v != std::string(100, 'a')) fail("snapshot read");
  ro.snapshot = nullptr;
  db->ReleaseSnapshot(snap);
  if (!db->Get(ro, KeyOf(0), &v).IsNotFound()) fail("deleted key visible");
  s = db->Get(ro, KeyOf(1), &v);
  if (!s.ok() || v != "updated") fail("updated value");
  db->CompactRange(nullptr, nullptr);
  env.WaitIdle();
  {
    std::unique_ptr<leveldb::Iterator> it(db->NewIterator(ro));
    int count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      if (it->value().ToString() != "updated") fail("iterator value after compaction");
      count++;
    }
    if (!it->status().ok() || count != n / 2) fail("iterator count after compaction");
    it->SeekToLast();
    if (!it->Valid() || it->key().ToString() != KeyOf(n - 1)) fail("SeekToLast");
    it->Seek(KeyOf(10000));
    if (!it->Valid() || it->key().ToString() != KeyOf(10001)) fail("Seek");
    it->Prev();
    if (!it->Valid() || it->key().ToString() != KeyOf(9999)) fail("Prev");
  }
  std::string prop;
  if (!db->GetProperty("leveldb.stats", &prop)) fail("GetProperty(leveldb.stats)");
  if (!db->GetProperty("leveldb.num-files-at-level0", &prop)) fail("GetProperty(level0)");
  if (!db->GetProperty("leveldb.sstables", &prop)) fail("GetProperty(sstables)");
  leveldb::Range range(KeyOf(0), KeyOf(n));
  uint64_t size = 0;
  db->GetApproximateSizes(&range, 1, &size);
  delete db;
  db = nullptr;
  env.WaitIdle();
  // Reopen and re-check.
  o.create_if_missing = false;
  s = leveldb::DB::Open(o, a.db, &db);
  if (!s.ok()) fail("reopen: " + s.ToString());
  env.WaitIdle();
  for (int i = 0; i < n; i += 3) {
    s = db->Get(ro, KeyOf(i), &v);
    if (i % 2 == 0) {
      if (!s.IsNotFound()) fail("deleted key visible after reopen");
    } else if (!s.ok() || v != "updated") {
      fail("value after reopen");
    }
  }
  delete db;
  fprintf(stderr, "selftest ok\n");
  return 0;
}

}  // namespace

// --stop-at-end: on success, SIGSTOP ourselves instead of exiting.
// measure.py reads /proc/<pid>/io while we're stopped and then kills us,
// so atexit handlers and static destructors never get to do any I/O.
void Finish(const Args& a, int code) {
  fflush(stdout);
  fflush(stderr);
  if (code == 0 && a.stop_at_end) {
    raise(SIGSTOP);
  }
  _exit(code);
}

int main(int argc, char** argv) {
  Args a = ParseArgs(argc, argv);
  if (a.mode == "describe") Finish(a, Describe(a));
  if (a.db.empty()) Usage();
  if (a.mode == "run") Finish(a, Run(a));
  if (a.mode == "read") Finish(a, Read(a));
  if (a.mode == "check") Finish(a, Check(a));
  if (a.mode == "selftest") Finish(a, SelfTest(a));
  Usage();
  return kExitUsage;
}
