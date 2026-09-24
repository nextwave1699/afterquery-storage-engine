#!/usr/bin/env python3
"""Check /app/leveldb the way the verifier does, on seeds it does not use.

    bench.py [--suite] [--perf] [--bench] [--quick] [--jobs N] [--seed-base N]
             [--work DIR] [--keep]

With no stage flag it runs everything (~5 min plus the stock benchmark):

  build        the candidate (and /opt/pristine) with the tree's CMake, and
               the harness in this directory against both
  suite        the API checks, 12000 scenarios with range deletes and merge
               operands (300 ops each), 400 long ones (3000 ops), 1000 stock
               scenarios, and 400 seeded crash-recovery cases
  perf         the four efficiency cases (spec.md section 5) against their
               limits
  bench        the stock workloads mixed, ttl and bimodal at scale 3 on both
               engines: write amplification at most 1.10x the pristine's,
               plus the guardrails in README.md, and cross reads

--quick shrinks the suite to a tenth and runs the benchmark at scale 1 (it
then only tells you whether things run; the limits are for scale 3).

The verifier runs the same counts on its own seeds.  A scenario that fails
here is reproducible on its own:

    /tmp/lsm-bench/conftest-cand scenarios --db /tmp/x --from SEED --to SEED+1 --ext [--ops N]

prints the first read that went wrong (see conf_model.h for what each
scenario does).  The score the verifier reports (feature_score) is the mean
over api, conformance, recovery and efficiency of the fraction of checks
passed; the gate needs every check, and the benchmark, to pass.
"""
import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import manifest  # noqa: E402

WORKLOADS = ["mixed", "ttl", "bimodal"]
WA_RATIO_MAX = 1.10
MAX_L0_DEPTH = 4
SPACE_RATIO_MAX = 1.25
DISK_RATIO_MAX = 1.5
READ_RATIO_MAX = 1.5
READ_RATIO_MAX_BY_WORKLOAD = {"ttl": 2.5}
FILES_RATIO_MAX = 3.0
MAX_FILE_SIZE = 8 * 1024 * 1024
RSS_EXTRA_MAX_KB = 96 * 1024
MEMTABLE_SWITCH_RATIO_MIN = 0.8
TIME_RATIO_MAX = 4.0

CONF_SCENARIOS, CONF_OPS = 12000, 300
CONF_LONG, CONF_LONG_OPS = 400, 3000
STOCK_SCENARIOS = 1000
RECOVERY_CASES = 400
PERF_LIMITS = {
    "rangedel": {"A_WCHAR": 512 << 10, "A_RCHAR": 512 << 10, "S_RCHAR": (0.5, 0),
                 "R_TABLE_BYTES": (0.3, 1 << 20), "B_TABLE_BYTES": (0.15, 1 << 20),
                 "VMHWM_KB": 1 << 20},
    "manyranges": {"M_CPU_MS": 15000, "VMHWM_KB": 1 << 20},
    "merge": {"A_WCHAR": 150 << 20, "A_RCHAR": 256 << 20, "B_TABLE_BYTES": (1.25, 0),
              "VMHWM_KB": 1 << 20},
    "cfwal": {"W_LOG_BYTES": 12 << 20, "W_WCHAR": 400 << 20, "VMHWM_KB": 1 << 20},
}


def sh(cmd, cwd=None, quiet=False, check=True):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if check and r.returncode != 0:
        print(r.stdout[-4000:])
        raise SystemExit("command failed: %s" % " ".join(cmd))
    if not quiet:
        sys.stdout.write(r.stdout)
    return r


def build_engine(tree, build_dir):
    os.makedirs(build_dir, exist_ok=True)
    if not os.path.exists(os.path.join(build_dir, "CMakeCache.txt")):
        sh(["cmake", "-DCMAKE_BUILD_TYPE=Release", "-DLEVELDB_BUILD_TESTS=OFF",
            "-DLEVELDB_BUILD_BENCHMARKS=OFF", "-DLEVELDB_INSTALL=OFF", tree], cwd=build_dir, quiet=True)
    sh(["make", "-j4", "leveldb"], cwd=build_dir, quiet=True)
    return os.path.join(build_dir, "libleveldb.a")


def build_tool(tree, lib, sources, out, extra=()):
    cmd = ["g++", "-std=c++17", "-O2", "-fno-rtti"] + list(extra) + [
        "-I" + os.path.join(tree, "include"), "-I" + HERE] + [os.path.join(HERE, s) for s in sources] + [
        "-o", out, lib, "-lpthread"]
    r = sh(cmd, quiet=True, check=False)
    if r.returncode != 0:
        print(r.stdout[-4000:])
        return None
    return out


# ---- suite -----------------------------------------------------------------

def run_scenarios(conf, work, name, lo, n, ops, ext, jobs):
    step = (n + jobs - 1) // jobs
    procs = []
    for i in range(jobs):
        a, b = lo + i * step, min(lo + n, lo + (i + 1) * step)
        if a >= b:
            continue
        db = os.path.join(work, "%s-%d" % (name, i))
        shutil.rmtree(db, ignore_errors=True)
        cmd = [conf, "scenarios", "--db", db, "--from", str(a), "--to", str(b), "--ops", str(ops), "--quiet"]
        if ext:
            cmd.append("--ext")
        procs.append(subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
    passed = 0
    fails = []
    for p in procs:
        out, _ = p.communicate()
        got = [l for l in out.splitlines() if l.startswith("PASSED ")]
        fails += [l for l in out.splitlines() if " FAIL " in l]
        if got:
            passed += int(got[-1].split()[1].split("/")[0])
        else:
            fails.append("a %s chunk died (status %s): %s" % (name, p.returncode, out[-300:]))
    for f in fails[:8]:
        print("    " + f[:400])
    if len(fails) > 8:
        print("    ... %d more" % (len(fails) - 8))
    ok = passed == n
    print("  %-26s %d/%d%s" % (name + " scenarios", passed, n, "" if ok else "  FAIL"))
    return passed, n


def run_recovery(cand, ref, work, cases, base):
    rng = random.Random(base)
    events = ["log_append", "table_append", "table_close", "table_sync", "manifest_append",
              "manifest_sync", "remove_file", "rename"]
    passed = 0
    for i in range(cases):
        seed = base + i
        ev = rng.choice(events)
        n = 1 + rng.randrange(3 if ev in ("rename", "manifest_sync") else 60)
        spec = "%s:%d%s" % (ev, n, ":after" if rng.random() < 0.3 else "")
        kind = ("ext", "ext", "ext", "stock", "pristine")[i % 5]
        if kind == "pristine" and ref is None:
            continue
        writer = ref if kind == "pristine" else cand
        ext = ["--ext"] if kind == "ext" else []
        db = os.path.join(work, "rec")
        shutil.rmtree(db, ignore_errors=True)
        ack = os.path.join(work, "rec.ack")
        r = subprocess.run([writer, "crash", "--db", db, "--seed", str(seed), "--crash", spec, "--ack", ack,
                            "--ops", str(CONF_OPS)] + ext, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True)
        if r.returncode not in (0, -9):
            print("    recovery %d (%s, %s): the run failed before crashing: %s" % (i, spec, kind, r.stdout[-300:]))
            continue
        acked = (open(ack).read().strip() if os.path.exists(ack) else "") or "0"
        states = []
        good = True
        readers = [("candidate", cand)] + ([("pristine", ref)] if kind == "stock" and ref else [])
        for tag, binary in readers:
            copy = db + "-" + tag
            shutil.rmtree(copy, ignore_errors=True)
            shutil.copytree(db, copy)
            p = subprocess.run([binary, "recover", "--db", copy, "--seed", str(seed), "--acked", acked,
                                "--ops", str(CONF_OPS)] + ext, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True)
            line = [l for l in p.stdout.splitlines() if l.startswith("RECOVERED ")]
            if p.returncode != 0 or not line:
                print("    recovery %d (seed %d, %s, %s, %s acked): %s recovery failed: %s" % (
                    i, seed, spec, kind, acked, tag, p.stdout.strip()[-300:]))
                good = False
                break
            states.append(line[0])
        if good and len(set(states)) > 1:
            print("    recovery %d (%s): engines disagree: %s" % (i, spec, states))
            good = False
        passed += good
    total = cases if ref else cases - cases // 5
    print("  %-26s %d/%d%s" % ("recovery cases", passed, total, "" if passed == total else "  FAIL"))
    return passed, total


def suite(cand, ref, args, fractions):
    print("\nsuite")
    db = os.path.join(args.work, "api")
    shutil.rmtree(db, ignore_errors=True)
    r = sh([cand, "api", "--db", db], quiet=True, check=False)
    lines = [l for l in r.stdout.splitlines() if l.startswith("API ")]
    for l in lines:
        if " FAIL" in l:
            print("    " + l)
    api_ok = sum(1 for l in lines if l.split()[2:3] == ["ok"])
    api_total = max(len(lines), 28)
    print("  %-26s %d/%d%s" % ("api checks", api_ok, api_total, "" if api_ok == api_total else "  FAIL"))
    fractions["api"] = api_ok / float(api_total)
    scale = 10 if args.quick else 1
    base = args.seed_base
    got = [run_scenarios(cand, args.work, "conf", base, CONF_SCENARIOS // scale, CONF_OPS, True, args.jobs),
           run_scenarios(cand, args.work, "long", base + 500000, CONF_LONG // scale, CONF_LONG_OPS, True, args.jobs),
           run_scenarios(cand, args.work, "stock", base + 700000, STOCK_SCENARIOS // scale, CONF_OPS, False,
                         args.jobs)]
    fractions["conformance"] = sum(p for p, _ in got) / float(sum(n for _, n in got))
    p, n = run_recovery(cand, ref, args.work, RECOVERY_CASES // scale, base + 900000)
    fractions["recovery"] = p / float(n) if n else 0.0


# ---- perf ------------------------------------------------------------------

def perf(cand, args, fractions):
    print("\nefficiency")
    passed = total = 0
    for case, limits in PERF_LIMITS.items():
        total += len(limits) + 1
        db = os.path.join(args.work, "perf-" + case)
        shutil.rmtree(db, ignore_errors=True)
        t = time.time()
        r = sh([cand, "perf", "--db", db, "--case", case], quiet=True, check=False)
        metrics = {}
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[0] == "METRIC":
                metrics[parts[1]] = int(parts[2])
        if r.returncode != 0 or "PERF ok" not in r.stdout:
            print("  %s: FAILED (status %d): %s" % (case, r.returncode, r.stdout[-500:]))
            continue
        passed += 1
        print("  %s (%.0fs): %s" % (case, time.time() - t, ", ".join("%s %d" % kv for kv in sorted(metrics.items()))))
        for key, limit in limits.items():
            if isinstance(limit, tuple):
                limit = int(limit[0] * metrics.get("LOAD_TABLE_BYTES", 0) + limit[1])
            val = metrics.get(key)
            ok = val is not None and val <= limit
            passed += ok
            print("    %-16s %12s  limit %12d  %s" % (key, val, limit, "ok" if ok else "FAIL"))
        shutil.rmtree(db, ignore_errors=True)
    fractions["efficiency"] = passed / float(total)


# ---- stock benchmark ---------------------------------------------------------

def measured(binary, mode, db, seed, scale, workload, work, tag):
    out = os.path.join(work, "meas-%s.json" % tag)
    cmd = ["python3", os.path.join(HERE, "measure.py"), out, "3600", "--",
           binary, mode, "--db", db, "--seed", str(seed), "--scale", str(scale), "--quiet",
           "--workload", workload]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    with open(out) as f:
        m = json.load(f)
    if not m.get("stopped"):
        print(r.stderr[-3000:])
        raise SystemExit("%s %s failed: %s" % (tag, mode, m))
    return m


def check(binary, db, seed, scale, workload, batches):
    r = subprocess.run([binary, "check", "--db", db, "--seed", str(seed), "--scale", str(scale),
                        "--workload", workload, "--batches", str(batches), "--paranoid", "--quiet"],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0 or ("APPLIED %d" % batches) not in r.stdout:
        print(r.stderr[-3000:])
        return False
    return True


def run_engine(tag, binary, workload, args, scale, logical):
    db = os.path.join(args.work, "db-%s-%s" % (workload, tag))
    shutil.rmtree(db, ignore_errors=True)
    run = measured(binary, "run", db, 1, scale, workload, args.work, "%s-%s-run" % (workload, tag))
    info = manifest.summarize(db)
    read = measured(binary, "read", db, 1, scale, workload, args.work, "%s-%s-read" % (workload, tag))
    res = {"wa": (run["wchar"] + read["wchar"]) / logical, "read_f": read["rchar"],
           "rss_kb": max(run["maxrss_kb"], read["maxrss_kb"]), "info": info,
           "wall": run["wall_s"] + read["wall_s"], "db": db}
    print("  %-9s WA %6.3f  (%.0fs)  levels %s  L0 depth %d  phase-F read %.0f MB  rss %d MB" % (
        tag, res["wa"], res["wall"], info["levels"], info["max_l0_depth"], read["rchar"] / 1e6,
        res["rss_kb"] // 1024))
    return res


def bench(cand, ref, args):
    scale = 1.0 if args.quick else 3.0
    all_ok = True
    for w in WORKLOADS:
        desc = json.loads(sh([cand, "describe", "--seed", "1", "--scale", str(scale), "--workload", w],
                             quiet=True).stdout.strip().splitlines()[-1])
        print("\n%s: scale %g, %d batches, %.1f MB logical" % (w, scale, desc["batches"], desc["logical_bytes"] / 1e6))
        b = run_engine("pristine", ref, w, args, scale, desc["logical_bytes"])
        c = run_engine("candidate", cand, w, args, scale, desc["logical_bytes"])
        cross = check(ref, c["db"], 1, scale, w, desc["batches"]) and check(cand, b["db"], 1, scale, w, desc["batches"])
        ic, ib = c["info"], b["info"]
        read_max = READ_RATIO_MAX_BY_WORKLOAD.get(w, READ_RATIO_MAX)
        rows = [
            ("cross reads", "ok" if cross else "FAIL", cross),
            ("WA <= %.2fx pristine" % WA_RATIO_MAX, "%.3fx" % (c["wa"] / b["wa"]), c["wa"] / b["wa"] <= WA_RATIO_MAX),
            ("level-0 depth <= %d" % MAX_L0_DEPTH, ic["max_l0_depth"], ic["max_l0_depth"] <= MAX_L0_DEPTH),
            ("largest table <= 8 MB", ic["max_file_size"], ic["max_file_size"] <= MAX_FILE_SIZE),
            ("peak space <= %.2fx" % SPACE_RATIO_MAX, "%.3fx" % (ic["max_total_bytes"] / ib["max_total_bytes"]),
             ic["max_total_bytes"] <= SPACE_RATIO_MAX * ib["max_total_bytes"]),
            ("disk after run <= %.1fx" % DISK_RATIO_MAX,
             "%.3fx" % (ic["table_bytes_present"] / max(1, ib["table_bytes_present"])),
             ic["table_bytes_present"] <= DISK_RATIO_MAX * ib["table_bytes_present"]),
            ("phase-F reads <= %.1fx" % read_max, "%.3fx" % (c["read_f"] / max(1, b["read_f"])),
             c["read_f"] <= read_max * b["read_f"]),
            ("peak files <= %.1fx" % FILES_RATIO_MAX, "%.3fx" % (ic["max_files"] / max(1, ib["max_files"])),
             ic["max_files"] <= FILES_RATIO_MAX * ib["max_files"]),
            ("memtable switches >= %.1fx" % MEMTABLE_SWITCH_RATIO_MIN,
             "%.3fx" % (ic["memtable_switches"] / max(1, ib["memtable_switches"])),
             ic["memtable_switches"] >= MEMTABLE_SWITCH_RATIO_MIN * ib["memtable_switches"]),
            ("peak RSS <= pristine + 96 MB", "+%d MB" % ((c["rss_kb"] - b["rss_kb"]) // 1024),
             c["rss_kb"] - b["rss_kb"] <= RSS_EXTRA_MAX_KB),
            ("time <= 4x pristine + 60 s", "%.2fx" % (c["wall"] / max(1.0, b["wall"])),
             c["wall"] <= TIME_RATIO_MAX * b["wall"] + 60),
        ]
        for name, val, ok in rows:
            all_ok = all_ok and ok
            print("    %-32s %-5s %s" % (name, "ok" if ok else "FAIL", val))
        if not args.keep:
            shutil.rmtree(c["db"], ignore_errors=True)
            shutil.rmtree(b["db"], ignore_errors=True)
    return all_ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--suite", action="store_true", help="API checks, scenarios and crash recovery")
    ap.add_argument("--perf", action="store_true", help="the efficiency cases")
    ap.add_argument("--bench", action="store_true", help="the stock-workload regression benchmark")
    ap.add_argument("--quick", action="store_true", help="a tenth of the suite, benchmark at scale 1")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--seed-base", type=int, default=3000000)
    ap.add_argument("--keep", action="store_true", help="keep the benchmark databases")
    ap.add_argument("--work", default="/tmp/lsm-bench")
    ap.add_argument("--tree", default="/app/leveldb")
    ap.add_argument("--pristine", default="/opt/pristine")
    args = ap.parse_args()
    if not (args.suite or args.perf or args.bench):
        args.suite = args.perf = args.bench = True
    os.makedirs(args.work, exist_ok=True)

    print("building candidate (%s)" % args.tree)
    cand_lib = build_engine(args.tree, os.path.join(args.work, "build-cand"))
    print("building pristine (%s)" % args.pristine)
    ref_lib = build_engine(args.pristine, os.path.join(args.work, "build-ref"))
    cconf = build_tool(args.tree, cand_lib, ["conftest.cc", "conf_model.cc"], os.path.join(args.work, "conftest-cand"))
    if cconf is None:
        print("the harness does not compile against %s/include: the extension API is missing or differs "
              "from spec.md" % args.tree)
    rconf = build_tool(args.pristine, ref_lib, ["conftest.cc", "conf_model.cc"],
                       os.path.join(args.work, "conftest-ref"), extra=["-DCONF_PRISTINE"])

    fractions = {"api": 0.0, "conformance": 0.0, "recovery": 0.0, "efficiency": 0.0}
    if cconf and args.suite:
        suite(cconf, rconf, args, fractions)
    if cconf and args.perf:
        perf(cconf, args, fractions)
    bench_ok = None
    if args.bench:
        cand = build_tool(args.tree, cand_lib, ["lsmbench.cc"], os.path.join(args.work, "lsmbench-cand"))
        ref = build_tool(args.pristine, ref_lib, ["lsmbench.cc"], os.path.join(args.work, "lsmbench-ref"))
        if cand is None:
            print("lsmbench does not compile against the candidate")
            bench_ok = False
        else:
            print("\nstock benchmark")
            bench_ok = bench(cand, ref, args)
    print()
    for k, v in fractions.items():
        print("  %-12s %.4f" % (k, v))
    if args.suite and args.perf:
        print("feature_score estimate: %.4f (1.0 = every check passed)" % (sum(fractions.values()) / 4))
    if bench_ok is not None:
        print("stock benchmark: %s" % ("ok" if bench_ok else "FAIL"))


if __name__ == "__main__":
    main()
