// BenchEnv: the leveldb::Env the benchmark harness hands to the engine.
//
// It is a complete POSIX Env of its own (it delegates nothing file-related to
// the engine's Env::Default()), so every byte the engine reads or writes goes
// through code owned by the harness:
//
//   * byte counters per file kind (WAL, table, manifest, other), reads and
//     writes, plus file create/remove counts;
//   * a deterministic scheduler: Env::Schedule() queues work on one worker
//     thread and the harness drains the queue after every operation, so
//     flushes and compactions happen at the same points on every machine and
//     the byte counts do not depend on timing;
//   * crash injection: an optional trigger (EVENT:N or EVENT:N:after) kills
//     the process with SIGKILL immediately before the N-th event of that kind
//     (or right after it completed).  Events are generic Env events -- WAL
//     append, table create/append/sync/close, manifest append/sync, file
//     removal, rename -- so they fire for any engine built on the Env API.
//
// Sync()/Flush() push user-space buffers to the OS but do not fsync: the
// crash model is a process kill, after which everything the OS accepted is
// still there, exactly as with a real process crash.
#ifndef LSMBENCH_BENCH_ENV_H_
#define LSMBENCH_BENCH_ENV_H_

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "leveldb/env.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace lsmbench {

enum FileKind { kLog = 0, kTable = 1, kManifest = 2, kOther = 3, kNumKinds = 4 };

inline FileKind ClassifyFile(const std::string& path) {
  size_t slash = path.rfind('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  auto ends_with = [&](const char* suf) {
    size_t n = strlen(suf);
    return base.size() >= n && base.compare(base.size() - n, n, suf) == 0;
  };
  if (ends_with(".log")) return kLog;
  if (ends_with(".ldb") || ends_with(".sst")) return kTable;
  if (base.compare(0, 9, "MANIFEST-") == 0) return kManifest;
  return kOther;
}

// Crash trigger events.
enum CrashEvent {
  kEvNone = 0,
  kEvLogAppend,
  kEvTableCreate,
  kEvTableAppend,
  kEvTableSync,
  kEvTableClose,
  kEvManifestAppend,
  kEvManifestSync,
  kEvRemoveFile,
  kEvRename,
  kEvOp,  // harness-level: before the N-th workload step
  kNumEvents
};

inline const char* CrashEventName(int e) {
  static const char* names[] = {"none",           "log_append",    "table_create",
                                "table_append",   "table_sync",    "table_close",
                                "manifest_append", "manifest_sync", "remove_file",
                                "rename",         "op"};
  return (e >= 0 && e < kNumEvents) ? names[e] : "?";
}

inline int ParseCrashEvent(const std::string& s) {
  for (int e = 1; e < kNumEvents; e++) {
    if (s == CrashEventName(e)) return e;
  }
  return kEvNone;
}

struct Counters {
  std::atomic<uint64_t> written[kNumKinds];
  std::atomic<uint64_t> read[kNumKinds];
  std::atomic<uint64_t> created[kNumKinds];
  std::atomic<uint64_t> removed[kNumKinds];
  std::atomic<uint64_t> syncs[kNumKinds];
  std::atomic<uint64_t> renames;
  Counters() {
    for (int k = 0; k < kNumKinds; k++) {
      written[k] = 0;
      read[k] = 0;
      created[k] = 0;
      removed[k] = 0;
      syncs[k] = 0;
    }
    renames = 0;
  }
  uint64_t total_written() const {
    uint64_t t = 0;
    for (int k = 0; k < kNumKinds; k++) t += written[k].load();
    return t;
  }
  uint64_t total_read() const {
    uint64_t t = 0;
    for (int k = 0; k < kNumKinds; k++) t += read[k].load();
    return t;
  }
};

class CrashInjector {
 public:
  CrashInjector() : event_(kEvNone), n_(0), after_(false), armed_(true), count_(0) {}

  // Events are only counted while armed; arming restarts the count.
  void Arm() {
    count_ = 0;
    armed_ = true;
  }
  void Disarm() { armed_ = false; }
  bool armed() const { return armed_; }

  // "EVENT:N" or "EVENT:N:after".  Returns false if unparsable.
  bool Configure(const std::string& spec) {
    if (spec.empty()) return true;
    size_t c1 = spec.find(':');
    if (c1 == std::string::npos) return false;
    std::string ev = spec.substr(0, c1);
    std::string rest = spec.substr(c1 + 1);
    size_t c2 = rest.find(':');
    std::string num = (c2 == std::string::npos) ? rest : rest.substr(0, c2);
    std::string mode = (c2 == std::string::npos) ? "" : rest.substr(c2 + 1);
    event_ = ParseCrashEvent(ev);
    if (event_ == kEvNone) return false;
    n_ = strtoull(num.c_str(), nullptr, 10);
    if (n_ == 0) return false;
    if (mode == "after") {
      after_ = true;
    } else if (!mode.empty()) {
      return false;
    }
    return true;
  }

  int event() const { return event_; }
  uint64_t n() const { return n_; }
  bool after() const { return after_; }
  uint64_t count() const { return count_.load(); }

  // Called before an event of kind `e` is performed.
  void Before(int e) {
    if (e == kEvNone || e != event_ || !armed_) return;
    uint64_t c = ++count_;
    if (!after_ && c == n_) Die();
  }
  // Called after an event of kind `e` completed.
  void After(int e) {
    if (e != event_ || !after_ || !armed_) return;
    if (count_.load() == n_) Die();
  }

 private:
  static void Die() {
    // Kill the whole process the hard way: no destructors, no buffered
    // flushes, exactly like a crash.
    kill(getpid(), SIGKILL);
    for (;;) pause();
  }
  int event_;
  uint64_t n_;
  bool after_;
  std::atomic<bool> armed_;
  std::atomic<uint64_t> count_;
};

class BenchEnv;

class BenchSequentialFile : public leveldb::SequentialFile {
 public:
  BenchSequentialFile(std::string fname, int fd, Counters* c, FileKind kind)
      : fname_(std::move(fname)), fd_(fd), counters_(c), kind_(kind) {}
  ~BenchSequentialFile() override { close(fd_); }

  leveldb::Status Read(size_t n, leveldb::Slice* result, char* scratch) override {
    while (true) {
      ssize_t r = ::read(fd_, scratch, n);
      if (r < 0) {
        if (errno == EINTR) continue;
        return leveldb::Status::IOError(fname_, strerror(errno));
      }
      counters_->read[kind_] += static_cast<uint64_t>(r);
      *result = leveldb::Slice(scratch, r);
      return leveldb::Status::OK();
    }
  }
  leveldb::Status Skip(uint64_t n) override {
    if (::lseek(fd_, n, SEEK_CUR) == static_cast<off_t>(-1)) {
      return leveldb::Status::IOError(fname_, strerror(errno));
    }
    return leveldb::Status::OK();
  }

 private:
  const std::string fname_;
  const int fd_;
  Counters* const counters_;
  const FileKind kind_;
};

class BenchRandomAccessFile : public leveldb::RandomAccessFile {
 public:
  BenchRandomAccessFile(std::string fname, int fd, Counters* c, FileKind kind)
      : fname_(std::move(fname)), fd_(fd), counters_(c), kind_(kind) {}
  ~BenchRandomAccessFile() override { close(fd_); }

  leveldb::Status Read(uint64_t offset, size_t n, leveldb::Slice* result,
                       char* scratch) const override {
    ssize_t r = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
    if (r < 0) {
      *result = leveldb::Slice(scratch, 0);
      return leveldb::Status::IOError(fname_, strerror(errno));
    }
    counters_->read[kind_] += static_cast<uint64_t>(r);
    *result = leveldb::Slice(scratch, r);
    return leveldb::Status::OK();
  }

 private:
  const std::string fname_;
  const int fd_;
  Counters* const counters_;
  const FileKind kind_;
};

class BenchWritableFile : public leveldb::WritableFile {
 public:
  static constexpr size_t kBufferSize = 65536;

  BenchWritableFile(std::string fname, int fd, Counters* c, CrashInjector* crash,
                    FileKind kind)
      : fname_(std::move(fname)), fd_(fd), pos_(0), counters_(c), crash_(crash),
        kind_(kind) {}
  ~BenchWritableFile() override {
    if (fd_ >= 0) Close();
  }

  leveldb::Status Append(const leveldb::Slice& data) override {
    int ev = AppendEvent();
    crash_->Before(ev);
    counters_->written[kind_] += data.size();
    size_t n = data.size();
    const char* p = data.data();
    size_t copy = std::min(n, kBufferSize - pos_);
    memcpy(buf_ + pos_, p, copy);
    p += copy;
    n -= copy;
    pos_ += copy;
    leveldb::Status s;
    if (n > 0) {
      s = FlushBuffer();
      if (s.ok()) {
        if (n < kBufferSize) {
          memcpy(buf_, p, n);
          pos_ = n;
        } else {
          s = WriteAll(p, n);
        }
      }
    }
    crash_->After(ev);
    return s;
  }

  leveldb::Status Close() override {
    int ev = (kind_ == kTable) ? kEvTableClose : kEvNone;
    crash_->Before(ev);
    leveldb::Status s = FlushBuffer();
    if (::close(fd_) < 0 && s.ok()) s = leveldb::Status::IOError(fname_, strerror(errno));
    fd_ = -1;
    crash_->After(ev);
    return s;
  }

  leveldb::Status Flush() override { return FlushBuffer(); }

  leveldb::Status Sync() override {
    int ev = (kind_ == kTable) ? kEvTableSync
             : (kind_ == kManifest) ? kEvManifestSync : kEvNone;
    crash_->Before(ev);
    counters_->syncs[kind_]++;
    // The data goes to the OS; an fsync is not needed for the process-crash
    // model and would only make the benchmark depend on the disk.
    leveldb::Status s = FlushBuffer();
    crash_->After(ev);
    return s;
  }

 private:
  int AppendEvent() const {
    switch (kind_) {
      case kLog: return kEvLogAppend;
      case kTable: return kEvTableAppend;
      case kManifest: return kEvManifestAppend;
      default: return kEvNone;
    }
  }
  leveldb::Status FlushBuffer() {
    leveldb::Status s = WriteAll(buf_, pos_);
    pos_ = 0;
    return s;
  }
  leveldb::Status WriteAll(const char* p, size_t n) {
    while (n > 0) {
      ssize_t w = ::write(fd_, p, n);
      if (w < 0) {
        if (errno == EINTR) continue;
        return leveldb::Status::IOError(fname_, strerror(errno));
      }
      p += w;
      n -= static_cast<size_t>(w);
    }
    return leveldb::Status::OK();
  }

  const std::string fname_;
  int fd_;
  char buf_[kBufferSize];
  size_t pos_;
  Counters* const counters_;
  CrashInjector* const crash_;
  const FileKind kind_;
};

class BenchFileLock : public leveldb::FileLock {
 public:
  BenchFileLock(int fd, std::string name) : fd_(fd), name_(std::move(name)) {}
  int fd_;
  std::string name_;
};

class BenchLogger : public leveldb::Logger {
 public:
  explicit BenchLogger(FILE* f) : f_(f) {}
  ~BenchLogger() override {
    if (f_ != nullptr) fclose(f_);
  }
  void Logv(const char* format, std::va_list ap) override {
    if (f_ == nullptr) return;
    struct timeval now;
    gettimeofday(&now, nullptr);
    fprintf(f_, "%llu.%06d ", static_cast<unsigned long long>(now.tv_sec),
            static_cast<int>(now.tv_usec));
    vfprintf(f_, format, ap);
    fputc('\n', f_);
    fflush(f_);
  }

 private:
  FILE* f_;
};

class BenchEnv : public leveldb::Env {
 public:
  BenchEnv()
      : pending_(0), stop_(false), worker_(&BenchEnv::WorkerLoop, this) {}

  ~BenchEnv() override {
    {
      std::lock_guard<std::mutex> l(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
  }

  Counters* counters() { return &counters_; }
  CrashInjector* crash() { return &crash_; }

  // ---- files ----
  leveldb::Status NewSequentialFile(const std::string& fname,
                                    leveldb::SequentialFile** result) override {
    int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      *result = nullptr;
      return IOErr(fname);
    }
    *result = new BenchSequentialFile(fname, fd, &counters_, ClassifyFile(fname));
    return leveldb::Status::OK();
  }

  leveldb::Status NewRandomAccessFile(const std::string& fname,
                                      leveldb::RandomAccessFile** result) override {
    int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      *result = nullptr;
      return IOErr(fname);
    }
    *result = new BenchRandomAccessFile(fname, fd, &counters_, ClassifyFile(fname));
    return leveldb::Status::OK();
  }

  leveldb::Status NewWritableFile(const std::string& fname,
                                  leveldb::WritableFile** result) override {
    FileKind kind = ClassifyFile(fname);
    int ev = (kind == kTable) ? kEvTableCreate : kEvNone;
    crash_.Before(ev);
    int fd = ::open(fname.c_str(), O_TRUNC | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      *result = nullptr;
      return IOErr(fname);
    }
    counters_.created[kind]++;
    *result = new BenchWritableFile(fname, fd, &counters_, &crash_, kind);
    crash_.After(ev);
    return leveldb::Status::OK();
  }

  leveldb::Status NewAppendableFile(const std::string& fname,
                                    leveldb::WritableFile** result) override {
    FileKind kind = ClassifyFile(fname);
    int fd = ::open(fname.c_str(), O_APPEND | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      *result = nullptr;
      return IOErr(fname);
    }
    *result = new BenchWritableFile(fname, fd, &counters_, &crash_, kind);
    return leveldb::Status::OK();
  }

  bool FileExists(const std::string& fname) override {
    return ::access(fname.c_str(), F_OK) == 0;
  }

  leveldb::Status GetChildren(const std::string& dir,
                              std::vector<std::string>* result) override {
    result->clear();
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return IOErr(dir);
    struct dirent* entry;
    while ((entry = ::readdir(d)) != nullptr) {
      result->emplace_back(entry->d_name);
    }
    ::closedir(d);
    return leveldb::Status::OK();
  }

  leveldb::Status RemoveFile(const std::string& fname) override {
    crash_.Before(kEvRemoveFile);
    leveldb::Status s;
    if (::unlink(fname.c_str()) != 0) {
      s = IOErr(fname);
    } else {
      counters_.removed[ClassifyFile(fname)]++;
    }
    crash_.After(kEvRemoveFile);
    return s;
  }

  leveldb::Status CreateDir(const std::string& dirname) override {
    if (::mkdir(dirname.c_str(), 0755) != 0) return IOErr(dirname);
    return leveldb::Status::OK();
  }

  leveldb::Status RemoveDir(const std::string& dirname) override {
    if (::rmdir(dirname.c_str()) != 0) return IOErr(dirname);
    return leveldb::Status::OK();
  }

  leveldb::Status GetFileSize(const std::string& fname, uint64_t* size) override {
    struct stat st;
    if (::stat(fname.c_str(), &st) != 0) {
      *size = 0;
      return IOErr(fname);
    }
    *size = st.st_size;
    return leveldb::Status::OK();
  }

  leveldb::Status RenameFile(const std::string& from, const std::string& to) override {
    crash_.Before(kEvRename);
    leveldb::Status s;
    if (::rename(from.c_str(), to.c_str()) != 0) {
      s = IOErr(from);
    } else {
      counters_.renames++;
    }
    crash_.After(kEvRename);
    return s;
  }

  leveldb::Status LockFile(const std::string& fname, leveldb::FileLock** lock) override {
    *lock = nullptr;
    int fd = ::open(fname.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return IOErr(fname);
    {
      std::lock_guard<std::mutex> l(locks_mu_);
      if (locks_.count(fname)) {
        ::close(fd);
        return leveldb::Status::IOError("lock " + fname, "already held by process");
      }
      locks_.insert(fname);
    }
    struct flock f;
    memset(&f, 0, sizeof(f));
    f.l_type = F_WRLCK;
    f.l_whence = SEEK_SET;
    if (::fcntl(fd, F_SETLK, &f) == -1) {
      int err = errno;
      ::close(fd);
      std::lock_guard<std::mutex> l(locks_mu_);
      locks_.erase(fname);
      return leveldb::Status::IOError("lock " + fname, strerror(err));
    }
    *lock = new BenchFileLock(fd, fname);
    return leveldb::Status::OK();
  }

  leveldb::Status UnlockFile(leveldb::FileLock* lock) override {
    BenchFileLock* l = static_cast<BenchFileLock*>(lock);
    struct flock f;
    memset(&f, 0, sizeof(f));
    f.l_type = F_UNLCK;
    f.l_whence = SEEK_SET;
    leveldb::Status s;
    if (::fcntl(l->fd_, F_SETLK, &f) == -1) s = IOErr(l->name_);
    {
      std::lock_guard<std::mutex> g(locks_mu_);
      locks_.erase(l->name_);
    }
    ::close(l->fd_);
    delete l;
    return s;
  }

  // ---- scheduling ----
  void Schedule(void (*function)(void*), void* arg) override {
    {
      std::lock_guard<std::mutex> l(mu_);
      queue_.emplace_back(function, arg);
      pending_++;
    }
    cv_.notify_all();
  }

  void StartThread(void (*function)(void*), void* arg) override {
    std::thread t(function, arg);
    t.detach();
  }

  // Block until every scheduled job has run (including jobs queued by the
  // jobs themselves).
  void WaitIdle() {
    std::unique_lock<std::mutex> l(mu_);
    idle_cv_.wait(l, [this] { return pending_ == 0; });
  }

  uint64_t jobs_run() const { return jobs_run_.load(); }

  leveldb::Status GetTestDirectory(std::string* path) override {
    *path = "/tmp";
    return leveldb::Status::OK();
  }

  leveldb::Status NewLogger(const std::string& fname, leveldb::Logger** result) override {
    // Never write the info log into the database directory (it would be
    // counted as bytes written); the harness installs its own logger.
    (void)fname;
    *result = new BenchLogger(nullptr);
    return leveldb::Status::OK();
  }

  uint64_t NowMicros() override {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  }

  void SleepForMicroseconds(int micros) override {
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
  }

 private:
  static leveldb::Status IOErr(const std::string& ctx) {
    if (errno == ENOENT) return leveldb::Status::NotFound(ctx, strerror(errno));
    return leveldb::Status::IOError(ctx, strerror(errno));
  }

  void WorkerLoop() {
    std::unique_lock<std::mutex> l(mu_);
    while (true) {
      cv_.wait(l, [this] { return stop_ || !queue_.empty(); });
      if (stop_ && queue_.empty()) return;
      auto job = queue_.front();
      queue_.pop_front();
      l.unlock();
      job.first(job.second);
      jobs_run_++;
      l.lock();
      pending_--;
      if (pending_ == 0) idle_cv_.notify_all();
    }
  }

  Counters counters_;
  CrashInjector crash_;
  std::mutex locks_mu_;
  std::set<std::string> locks_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable idle_cv_;
  std::deque<std::pair<void (*)(void*), void*>> queue_;
  int pending_;
  bool stop_;
  std::atomic<uint64_t> jobs_run_{0};
  std::thread worker_;
};

}  // namespace lsmbench

#endif  // LSMBENCH_BENCH_ENV_H_
