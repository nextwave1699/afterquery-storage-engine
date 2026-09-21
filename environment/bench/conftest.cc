// conftest: the conformance and recovery suite, over LevelDB's public API.
//
//   conftest scenarios --db DIR --from A --to B [--ops N] [--quiet]
//       run scenario seeds [A, B), each in a fresh directory under DIR;
//       prints "SCENARIO <seed> ok" or "SCENARIO <seed> FAIL <what>" and
//       finally "PASSED <passed>/<total>"
//   conftest crash --db DIR --seed S --crash EVENT:N[:after] --ack FILE [--ops N]
//       run one scenario with a crash armed (see bench_env.h); FILE holds
//       the number of acknowledged write operations.  If the event never
//       comes, the scenario simply finishes.
//   conftest recover --db DIR --seed S --acked K [--ops N]
//       open the database a crash left behind and check it holds exactly the
//       model after K writes, or after K+1 (the write in flight), then that
//       it takes new writes and survives a reopen.  Prints "RECOVERED <n>".
//
// Scenarios are described in conf_model.h.  Exit status: 0 ok, 2 a check
// failed, 3 engine error, 4 usage.
#include <fcntl.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "conf_model.h"

namespace {

const int kExitMismatch = 2;
const int kExitUsage = 4;

struct Args {
  std::string mode;
  std::string db;
  std::string crash;
  std::string ack;
  uint64_t from = 0, to = 1, seed = 0, acked = 0;
  uint64_t ops = 300;
  bool quiet = false;
};

void Usage() {
  fprintf(stderr,
          "usage: conftest (scenarios|crash|recover) --db DIR [--from A --to B] [--seed S] "
          "[--ops N] [--crash SPEC --ack FILE] [--acked K] [--quiet]\n");
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
    else if (k == "--quiet") a.quiet = true;
    else Usage();
  }
  if (a.db.empty()) Usage();
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace lsmbench;
  Args a = ParseArgs(argc, argv);

  if (a.mode == "crash") {
    if (a.crash.empty() || a.ack.empty()) Usage();
    int fd = open(a.ack.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) Usage();
    std::string err;
    bool ok = RunScenario(a.db, a.seed, a.ops, a.crash, fd, &err);
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
    if (!CheckRecovered(a.db, a.seed, a.ops, a.acked, &recovered, &err)) {
      fprintf(stderr, "RECOVERY %" PRIu64 " FAIL %s\n", a.seed, err.c_str());
      return kExitMismatch;
    }
    printf("RECOVERED %" PRIu64 "\n", recovered);
    return 0;
  }

  if (a.mode != "scenarios") Usage();
  uint64_t passed = 0, total = 0;
  for (uint64_t seed = a.from; seed < a.to; seed++) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/s%" PRIu64, a.db.c_str(), seed);
    std::string err;
    total++;
    if (RunScenario(dir, seed, a.ops, "", -1, &err)) {
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
