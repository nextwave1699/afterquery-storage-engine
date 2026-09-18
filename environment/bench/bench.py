#!/usr/bin/env python3
"""Estimate the verifier's measurements for the tree in /app/leveldb.

    bench.py [--seed N] [--scale X] [--candidate-only] [--keep]

Builds the untouched engine in /opt/pristine (once) and the candidate in
/app/leveldb, compiles the harness (lsmbench.cc, this directory) against
each, runs the fixed workload with each (phases A..E in one process, phase F
in a second one), reads the kernel's byte counters of those processes, checks
the candidate's database with the pristine reader and vice versa, replays
the MANIFEST history for the shape guardrails, and prints the numbers the
verifier would look at.  Defaults: seed 1, scale 3 (the verifier's scale;
its seed is drawn at verification time).

This is an estimate, not the score: the verifier also runs crash-recovery
scenarios and compatibility fixtures, on its own machine.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import manifest  # noqa: E402

WA_MAX = 5.40
MAX_L0_DEPTH = 4
SPACE_RATIO_MAX = 1.25
READ_RATIO_MAX = 1.5
FILES_RATIO_MAX = 3.0
MAX_FILE_SIZE = 8 * 1024 * 1024
RSS_EXTRA_MAX_KB = 96 * 1024
MEMTABLE_SWITCH_RATIO_MIN = 0.8


def sh(cmd, cwd=None, quiet=False):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:])
        raise SystemExit("command failed: %s" % " ".join(cmd))
    if not quiet:
        sys.stdout.write(r.stdout)
    return r.stdout


def build(tree, build_dir, binary):
    os.makedirs(build_dir, exist_ok=True)
    if not os.path.exists(os.path.join(build_dir, "CMakeCache.txt")):
        sh(["cmake", "-DCMAKE_BUILD_TYPE=Release", "-DLEVELDB_BUILD_TESTS=OFF",
            "-DLEVELDB_BUILD_BENCHMARKS=OFF", "-DLEVELDB_INSTALL=OFF", tree], cwd=build_dir, quiet=True)
    sh(["make", "-j4", "leveldb"], cwd=build_dir, quiet=True)
    sh(["g++", "-std=c++17", "-O2", "-fno-rtti", "-I" + os.path.join(tree, "include"), "-I" + HERE,
        os.path.join(HERE, "lsmbench.cc"), "-o", binary, os.path.join(build_dir, "libleveldb.a"), "-lpthread"],
       quiet=True)
    return binary


def measured(binary, mode, db, seed, scale, work, tag):
    out = os.path.join(work, "meas-%s.json" % tag)
    cmd = ["python3", os.path.join(HERE, "measure.py"), out, "3600", "--",
           binary, mode, "--db", db, "--seed", str(seed), "--scale", str(scale), "--quiet"]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    with open(out) as f:
        m = json.load(f)
    if not m.get("stopped"):
        print(r.stderr[-3000:])
        raise SystemExit("%s %s failed: %s" % (tag, mode, m))
    return m


def check(binary, db, seed, scale, batches):
    r = subprocess.run([binary, "check", "--db", db, "--seed", str(seed), "--scale", str(scale),
                        "--batches", str(batches), "--paranoid", "--quiet"],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0 or ("APPLIED %d" % batches) not in r.stdout:
        print(r.stderr[-3000:])
        raise SystemExit("check failed on %s" % db)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--scale", type=float, default=3.0)
    ap.add_argument("--candidate-only", action="store_true")
    ap.add_argument("--keep", action="store_true", help="keep the databases in the work directory")
    ap.add_argument("--work", default="/tmp/lsm-bench")
    ap.add_argument("--tree", default="/app/leveldb")
    ap.add_argument("--pristine", default="/opt/pristine")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)

    print("building candidate (%s)" % args.tree)
    cand = build(args.tree, os.path.join(args.work, "build-cand"), os.path.join(args.work, "lsmbench-cand"))
    ref = None
    if not args.candidate_only:
        print("building pristine (%s)" % args.pristine)
        ref = build(args.pristine, os.path.join(args.work, "build-ref"), os.path.join(args.work, "lsmbench-ref"))

    desc = json.loads(sh([cand, "describe", "--seed", str(args.seed), "--scale", str(args.scale)], quiet=True).strip())
    logical = desc["logical_bytes"]
    print("workload: seed %d scale %g: %d batches, %.1f MB logical, %d live keys at the end" % (
        args.seed, args.scale, desc["batches"], logical / 1e6, desc["live_keys"]))

    results = {}
    for tag, binary in (("pristine", ref), ("candidate", cand)):
        if binary is None:
            continue
        db = os.path.join(args.work, "db-" + tag)
        shutil.rmtree(db, ignore_errors=True)
        run = measured(binary, "run", db, args.seed, args.scale, args.work, tag + "-run")
        info = manifest.summarize(db)
        read = measured(binary, "read", db, args.seed, args.scale, args.work, tag + "-read")
        written = run["wchar"] + read["wchar"]
        results[tag] = {"wa": written / logical, "written": written, "read_f": read["rchar"],
                        "rss_kb": max(run["maxrss_kb"], read["maxrss_kb"]), "info": info,
                        "wall": run["wall_s"] + read["wall_s"], "db": db}
        print("%-9s write amplification %.3f  (%.0f MB written; run %.1fs)  levels at end %s  L0 depth max %d  "
              "peak space %.0f MB  peak files %d  memtable switches %d  phase-F read %.0f MB  rss %d MB" % (
                  tag, written / logical, written / 1e6, run["wall_s"], info["levels"], info["max_l0_depth"],
                  info["max_total_bytes"] / 1e6, info["max_files"], info["memtable_switches"],
                  read["rchar"] / 1e6, results[tag]["rss_kb"] // 1024))
    if ref is not None:
        print("cross reads: pristine reads the candidate's database, candidate reads the pristine's ...")
        check(ref, results["candidate"]["db"], args.seed, args.scale, desc["batches"])
        check(cand, results["pristine"]["db"], args.seed, args.scale, desc["batches"])
        print("cross reads ok")
        b, c = results["pristine"], results["candidate"]
        ib, ic = b["info"], c["info"]
        checks = [
            ("write amplification <= %.2f" % WA_MAX, c["wa"] <= WA_MAX, "%.3f" % c["wa"]),
            ("level-0 depth <= %d" % MAX_L0_DEPTH, ic["max_l0_depth"] <= MAX_L0_DEPTH, str(ic["max_l0_depth"])),
            ("peak space <= %.2fx" % SPACE_RATIO_MAX, ic["max_total_bytes"] <= SPACE_RATIO_MAX * ib["max_total_bytes"],
             "%.2fx" % (ic["max_total_bytes"] / max(1, ib["max_total_bytes"]))),
            ("phase-F read bytes <= %.1fx" % READ_RATIO_MAX, c["read_f"] <= READ_RATIO_MAX * b["read_f"],
             "%.2fx" % (c["read_f"] / max(1, b["read_f"]))),
            ("peak file count <= %.1fx" % FILES_RATIO_MAX, ic["max_files"] <= FILES_RATIO_MAX * ib["max_files"],
             "%.2fx" % (ic["max_files"] / max(1, ib["max_files"]))),
            ("largest table <= %d MB" % (MAX_FILE_SIZE >> 20), ic["max_file_size"] <= MAX_FILE_SIZE,
             "%.1f MB" % (ic["max_file_size"] / 2**20)),
            ("peak RSS <= baseline + %d MB" % (RSS_EXTRA_MAX_KB >> 10), c["rss_kb"] - b["rss_kb"] <= RSS_EXTRA_MAX_KB,
             "+%d MB" % ((c["rss_kb"] - b["rss_kb"]) // 1024)),
            ("memtable switches >= %.1fx" % MEMTABLE_SWITCH_RATIO_MIN,
             ic["memtable_switches"] >= MEMTABLE_SWITCH_RATIO_MIN * ib["memtable_switches"],
             "%.2fx" % (ic["memtable_switches"] / max(1, ib["memtable_switches"]))),
        ]
        print()
        for name, ok, val in checks:
            print("  %-36s %-8s %s" % (name, "ok" if ok else "FAIL", val))
        print("\nestimate: %s (baseline write amplification %.3f, reduction %.0f%%)" % (
            "would pass" if all(ok for _, ok, _ in checks) else "would NOT pass",
            b["wa"], 100 * (1 - c["wa"] / b["wa"])))
        if abs(args.scale - 3.0) > 1e-9:
            print("note: the bar applies at scale 3; write amplification is lower at smaller scales "
                  "(fewer levels), so only a scale-3 run is comparable")
    if not args.keep:
        for r in results.values():
            shutil.rmtree(r["db"], ignore_errors=True)


if __name__ == "__main__":
    main()
