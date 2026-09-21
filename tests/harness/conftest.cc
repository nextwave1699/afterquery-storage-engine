// conftest: model-checked scenarios for range deletes and merge operands.
//
//   conftest scenarios --db DIR --from A --to B [--ops N] [--quiet]
//       run scenario seeds [A, B) one after another in fresh directories
//       under DIR, printing "SCENARIO <seed> ok|FAIL <what>" for each and
//       "PASSED n/m" at the end
//   conftest one --db DIR --seed S [--ops N] [--verbose]
//       run a single scenario, leaving the database behind
//   conftest perf --db DIR --case rangedel|merge [--scale N]
//       load a database, then delete wide ranges of it or record many merge
//       operands, printing PHASE_WCHAR/PHASE_RCHAR for that phase alone
//
// A scenario is a pure function of its seed: a sequence of puts, deletes,
// range deletes, merges, gets, iterations, snapshot reads, reopens and
// compactions over a small key space, with every read checked against a
// model of what the database should hold.  Exit status is 0 when every
// scenario passed, 2 when one of them failed, 3 on an engine error.
#include <cinttypes>
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

#include "conf_model.h"

namespace lsmbench {
namespace {

const int kExitMismatch = 2;
const int kExitEngine = 3;
const int kExitUsage = 4;

struct Args {
  std::string mode;
  std::string db;
  uint64_t from = 0, to = 1, seed = 0;
  uint64_t ops = 400;
  uint64_t scale = 1;
  std::string which;
  bool quiet = false;
  bool verbose = false;
};

void Usage() {
  fprintf(stderr,
          "usage: conftest (scenarios|one) --db DIR [--from A --to B | --seed S] "
          "[--ops N] [--quiet] [--verbose]\n");
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
    else if (k == "--scale") a.scale = strtoull(need().c_str(), nullptr, 10);
    else if (k == "--case") a.which = need();
    else if (k == "--quiet") a.quiet = true;
    else if (k == "--verbose") a.verbose = true;
    else Usage();
  }
  if (a.db.empty()) Usage();
  return a;
}

}  // namespace
}  // namespace lsmbench

int main(int argc, char** argv) {
  using namespace lsmbench;
  Args a = ParseArgs(argc, argv);
  if (a.mode == "one") {
    std::string err;
    bool ok = RunScenario(a.db, a.seed, a.ops, a.verbose, &err);
    if (!ok) {
      fprintf(stderr, "SCENARIO %" PRIu64 " FAIL %s\n", a.seed, err.c_str());
      return kExitMismatch;
    }
    printf("SCENARIO %" PRIu64 " ok\n", a.seed);
    return 0;
  }
  if (a.mode == "perf") {
    if (a.which.empty()) Usage();
    std::string err;
    if (!RunPerfCase(a.db, a.which, a.scale, &err)) {
      fprintf(stderr, "perf %s: %s\n", a.which.c_str(), err.c_str());
      return kExitEngine;
    }
    return 0;
  }
  if (a.mode != "scenarios") Usage();
  uint64_t passed = 0, total = 0;
  for (uint64_t seed = a.from; seed < a.to; seed++) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/s%" PRIu64, a.db.c_str(), seed);
    std::string err;
    total++;
    if (RunScenario(dir, seed, a.ops, false, &err)) {
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
