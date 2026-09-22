// conftest: the conformance and recovery suite, over LevelDB's public API.
//
//   conftest scenarios --db DIR --from A --to B [--ops N] [--ext] [--quiet]
//       run scenario seeds [A, B), each in a fresh directory under DIR;
//       prints "SCENARIO <seed> ok" or "SCENARIO <seed> FAIL <what>" and
//       finally "PASSED <passed>/<total>"
//   conftest crash --db DIR --seed S --crash EVENT:N[:after] --ack FILE [--ops N] [--ext]
//       run one scenario with a crash armed (see bench_env.h); FILE holds
//       the number of acknowledged write operations.  If the event never
//       comes, the scenario simply finishes.
//   conftest recover --db DIR --seed S --acked K [--ops N] [--ext]
//       open the database a crash left behind and check it holds exactly the
//       model after K writes, or after K+1 (the write in flight), then that
//       it takes new writes and survives a reopen.  Prints "RECOVERED <n>".
//   conftest api --db DIR
//       fixed checks of the range-delete and merge API ("API <name> ok|FAIL")
//   conftest perf --db DIR --case rangedel|merge [--scale X]
//       an efficiency case; prints "METRIC <name> <value>" lines
//
// --ext turns on range deletes and merge operands in the scenarios (see
// conf_model.h).  Built with -DCONF_PRISTINE (for the untouched engine,
// which has neither) only the stock scenarios exist.
//
// Exit status: 0 ok, 2 a check failed, 3 engine error, 4 usage.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "conf_model.h"

namespace {

const int kExitMismatch = 2;
const int kExitEngine = 3;
const int kExitUsage = 4;

struct Args {
  std::string mode;
  std::string db;
  std::string crash;
  std::string ack;
  std::string which;
  uint64_t from = 0, to = 1, seed = 0, acked = 0;
  uint64_t ops = 300;
  double scale = 1.0;
  bool ext = false;
  bool quiet = false;
};

void Usage() {
  fprintf(stderr,
          "usage: conftest (scenarios|crash|recover|api|perf) --db DIR [--from A --to B] [--seed S]\n"
          "       [--ops N] [--ext] [--crash SPEC --ack FILE] [--acked K] [--case NAME] [--scale X]\n"
          "       [--quiet]\n");
  exit(kExitUsage);
}

Args ParseArgs(int argc, char** argv) {
  Args a;
  if (argc < 2) Usage();
  a.mode = argv[1];
  for (int i = 2; i < argc; i++) {
    std::string k = argv[i];
    auto need = [&]() -> std::string {
      if (i + 1 >= argc) Usage();
      return argv[++i];
    };
    if (k == "--db") a.db = need();
    else if (k == "--from") a.from = strtoull(need().c_str(), nullptr, 10);
    else if (k == "--to") a.to = strtoull(need().c_str(), nullptr, 10);
    else if (k == "--seed") a.seed = strtoull(need().c_str(), nullptr, 10);
    else if (k == "--ops") a.ops = strtoull(need().c_str(), nullptr, 10);
    else if (k == "--acked") a.acked = strtoull(need().c_str(), nullptr, 10);
    else if (k == "--crash") a.crash = need();
    else if (k == "--ack") a.ack = need();
    else if (k == "--case") a.which = need();
    else if (k == "--scale") a.scale = strtod(need().c_str(), nullptr);
    else if (k == "--ext") a.ext = true;
    else if (k == "--quiet") a.quiet = true;
    else Usage();
  }
  if (a.db.empty()) Usage();
#ifdef CONF_PRISTINE
  if (a.ext || a.mode == "api" || a.mode == "perf") {
    fprintf(stderr, "conftest: built without the extensions\n");
    exit(kExitUsage);
  }
#endif
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace lsmbench;
  Args a = ParseArgs(argc, argv);
  if (a.mode == "scenarios" || a.mode == "api") mkdir(a.db.c_str(), 0755);

  if (a.mode == "crash") {
    if (a.crash.empty() || a.ack.empty()) Usage();
    int fd = open(a.ack.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) Usage();
    std::string err;
    bool ok = RunScenario(a.db, a.seed, a.ops, a.ext, a.crash, fd, &err);
    if (!ok) {
      fprintf(stderr, "SCENARIO %" PRIu64 " FAIL %s\n", a.seed, err.c_str());
      return kExitMismatch;
    }
    printf("SCENARIO %" PRIu64 " finished without crashing\n", a.seed);
    return 0;
  }

  if (a.mode == "recover") {
    uint64_t recovered = 0;
    std::string err;
    if (!CheckRecovered(a.db, a.seed, a.ops, a.ext, a.acked, &recovered, &err)) {
      fprintf(stderr, "RECOVERY %" PRIu64 " FAIL %s\n", a.seed, err.c_str());
      return kExitMismatch;
    }
    printf("RECOVERED %" PRIu64 "\n", recovered);
    return 0;
  }

#ifndef CONF_PRISTINE
  if (a.mode == "api") {
    int passed = 0, total = 0;
    bool ok = RunApiChecks(a.db, &passed, &total);
    printf("PASSED %d/%d\n", passed, total);
    return ok ? 0 : kExitMismatch;
  }

  if (a.mode == "perf") {
    if (a.which.empty()) Usage();
    std::string err;
    if (!RunPerfCase(a.db, a.which, a.scale, &err)) {
      fprintf(stderr, "perf %s: %s\n", a.which.c_str(), err.c_str());
      printf("PERF FAIL %s\n", err.c_str());
      return kExitEngine;
    }
    printf("PERF ok\n");
    return 0;
  }
#endif

  if (a.mode != "scenarios") Usage();
  uint64_t passed = 0, total = 0;
  for (uint64_t seed = a.from; seed < a.to; seed++) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/s%" PRIu64, a.db.c_str(), seed);
    std::string err;
    total++;
    if (RunScenario(dir, seed, a.ops, a.ext, "", -1, &err)) {
      passed++;
      if (!a.quiet) printf("SCENARIO %" PRIu64 " ok\n", seed);
    } else {
      printf("SCENARIO %" PRIu64 " FAIL %s\n", seed, err.c_str());
    }
    fflush(stdout);
    DestroyScenarioDir(dir);
  }
  printf("PASSED %" PRIu64 "/%" PRIu64 "\n", passed, total);
  fflush(stdout);
  return passed == total ? 0 : kExitMismatch;
}
