#!/usr/bin/env python3
"""Measure /app/leveldb against /opt/pristine the way the verifier does.

    bench.py [--workload W ...] [--seed N] [--scale X] [--candidate-only] [--keep]

Builds both engines (pristine only once) with lsmbench from this directory,
runs every scored workload (mixed, uniform, series, blob) on each, cross-reads
the databases and prints the guardrails and an estimated score. Defaults to
seed 1 at scale 3, which is the verifier's scale; a full run takes a few
minutes. --workload limits the run to some workloads while you iterate.

The verifier scores a fixed seed of its own. The pristine engine's numbers on
that seed are in scored_baseline.json. A candidate's write amplification
barely moves with the seed while the pristine's does, so candidate WA / that
pristine WA is the estimate to trust, and every ratio guardrail is checked
against both the local pristine run and scored_baseline.json (the worse one
is shown). --candidate-only skips the pristine build and checks against
scored_baseline.json alone (no RSS or time check).

The score is the geometric mean of the per-workload ratios; a guardrail
failure on any workload means the score does not count.
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import manifest  # noqa: E402

WORKLOADS = ["mixed", "uniform", "series", "blob"]
MAX_L0_DEPTH = 4
SPACE_RATIO_MAX = 1.25
DISK_RATIO_MAX = 1.5
READ_RATIO_MAX = 1.5
FILES_RATIO_MAX = 3.0
MAX_FILE_SIZE = 8 * 1024 * 1024
RSS_EXTRA_MAX_KB = 96 * 1024
MEMTABLE_SWITCH_RATIO_MIN = 0.8
TIME_RATIO_MAX = 4.0


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
        raise SystemExit("check failed on %s" % db)


def run_engine(tag, binary, workload, args, logical):
    db = os.path.join(args.work, "db-%s-%s" % (workload, tag))
    shutil.rmtree(db, ignore_errors=True)
    run = measured(binary, "run", db, args.seed, args.scale, workload, args.work, "%s-%s-run" % (workload, tag))
    info = manifest.summarize(db)
    read = measured(binary, "read", db, args.seed, args.scale, workload, args.work, "%s-%s-read" % (workload, tag))
    written = run["wchar"] + read["wchar"]
    res = {"wa": written / logical, "read_f": read["rchar"], "rss_kb": max(run["maxrss_kb"], read["maxrss_kb"]),
           "info": info, "wall": run["wall_s"] + read["wall_s"], "db": db}
    print("  %-9s WA %6.3f  (run %.1fs)  levels %s  L0 depth %d  peak space %.0f MB  peak files %d  "
          "memtable switches %d  phase-F read %.0f MB  rss %d MB" % (
              tag, res["wa"], run["wall_s"], info["levels"], info["max_l0_depth"], info["max_total_bytes"] / 1e6,
              info["max_files"], info["memtable_switches"], read["rchar"] / 1e6, res["rss_kb"] // 1024))
    return res


def guardrails(c, b, sb):
    """(name, value, ok, format) per guardrail. Ratios are taken against the
    local pristine run `b` and the scored seed's pristine `sb`, whichever is
    worse; either may be None."""
    ic = c["info"]

    def worst(val, local, scored, higher_is_worse=True):
        ratios = []
        if b is not None:
            ratios.append(val / max(1, local(b)))
        if sb is not None:
            ratios.append(val / max(1, sb[scored]))
        if not ratios:
            return None
        return max(ratios) if higher_is_worse else min(ratios)

    out = [("level-0 depth <= %d" % MAX_L0_DEPTH, ic["max_l0_depth"], ic["max_l0_depth"] <= MAX_L0_DEPTH, "%d"),
           ("largest table <= %d MB" % (MAX_FILE_SIZE >> 20), ic["max_file_size"] / 2**20,
            ic["max_file_size"] <= MAX_FILE_SIZE, "%.1f MB")]
    for name, val, limit in (
            ("peak space <= %.2fx" % SPACE_RATIO_MAX,
             worst(ic["max_total_bytes"], lambda r: r["info"]["max_total_bytes"], "max_total_bytes"), SPACE_RATIO_MAX),
            ("disk after run <= %.1fx" % DISK_RATIO_MAX,
             worst(ic["table_bytes_present"], lambda r: r["info"]["table_bytes_present"], "table_bytes_present"),
             DISK_RATIO_MAX),
            ("phase-F read bytes <= %.1fx" % READ_RATIO_MAX,
             worst(c["read_f"], lambda r: r["read_f"], "read_bytes_f"), READ_RATIO_MAX),
            ("peak file count <= %.1fx" % FILES_RATIO_MAX,
             worst(ic["max_files"], lambda r: r["info"]["max_files"], "max_files"), FILES_RATIO_MAX)):
        if val is not None:
            out.append((name, val, val <= limit, "%.3fx"))
    val = worst(ic["memtable_switches"], lambda r: r["info"]["memtable_switches"], "memtable_switches", False)
    if val is not None:
        out.append(("memtable switches >= %.1fx" % MEMTABLE_SWITCH_RATIO_MIN, val,
                    val >= MEMTABLE_SWITCH_RATIO_MIN, "%.3fx"))
    if b is not None:
        extra = c["rss_kb"] - b["rss_kb"]
        out.append(("peak RSS <= baseline + %d MB" % (RSS_EXTRA_MAX_KB >> 10), extra // 1024,
                    extra <= RSS_EXTRA_MAX_KB, "+%d MB"))
        out.append(("wall time <= %.0fx baseline + 60 s" % TIME_RATIO_MAX, c["wall"] / max(1.0, b["wall"]),
                    c["wall"] <= TIME_RATIO_MAX * b["wall"] + 60, "%.2fx"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workload", action="append", choices=WORKLOADS,
                    help="run only this workload (repeatable; default: all)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--scale", type=float, default=3.0)
    ap.add_argument("--candidate-only", action="store_true")
    ap.add_argument("--keep", action="store_true", help="keep the databases in the work directory")
    ap.add_argument("--work", default="/tmp/lsm-bench")
    ap.add_argument("--tree", default="/app/leveldb")
    ap.add_argument("--pristine", default="/opt/pristine")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    workloads = args.workload or WORKLOADS

    print("building candidate (%s)" % args.tree)
    cand = build(args.tree, os.path.join(args.work, "build-cand"), os.path.join(args.work, "lsmbench-cand"))
    ref = None
    if not args.candidate_only:
        print("building pristine (%s)" % args.pristine)
        ref = build(args.pristine, os.path.join(args.work, "build-ref"), os.path.join(args.work, "lsmbench-ref"))

    full = abs(args.scale - 3.0) < 1e-9
    scored = None
    if full:
        with open(os.path.join(HERE, "scored_baseline.json")) as f:
            scored = json.load(f)
    estimates, local_ratios, all_ok = {}, {}, True
    for w in workloads:
        desc = json.loads(sh([cand, "describe", "--seed", str(args.seed), "--scale", str(args.scale),
                              "--workload", w], quiet=True).strip())
        print("\n%s: seed %d scale %g, %d batches, %.1f MB logical" % (
            w, args.seed, args.scale, desc["batches"], desc["logical_bytes"] / 1e6))
        b = run_engine("pristine", ref, w, args, desc["logical_bytes"]) if ref else None
        c = run_engine("candidate", cand, w, args, desc["logical_bytes"])
        if b is not None:
            check(ref, c["db"], args.seed, args.scale, w, desc["batches"])
            check(cand, b["db"], args.seed, args.scale, w, desc["batches"])
            print("  cross reads ok")
        sb = scored[w] if scored else None
        for name, val, ok, fmt in guardrails(c, b, sb):
            all_ok = all_ok and ok
            print("    %-36s %-5s %s" % (name, "ok" if ok else "FAIL", fmt % val))
        if sb is not None:
            estimates[w] = c["wa"] / sb["wa"]
        if b is not None:
            local_ratios[w] = c["wa"] / b["wa"]
        print("  ratio: %s%s" % (
            ("estimated %.4f (vs scored-seed pristine WA %.4f)" % (estimates[w], sb["wa"])) if w in estimates else "",
            ("; seed %d only %.4f" % (args.seed, local_ratios[w])) if w in local_ratios else ""))
        if not args.keep:
            shutil.rmtree(c["db"], ignore_errors=True)
            if b is not None:
                shutil.rmtree(b["db"], ignore_errors=True)

    geo = lambda d: math.exp(sum(math.log(v) for v in d.values()) / len(d))
    print("\nguardrails: %s" % ("ok" if all_ok else "FAIL (the score would not count)"))
    if estimates:
        print("estimated score: %.4f  (geometric mean over %s; untouched = 1.0, lower is better)" % (
            geo(estimates), ", ".join(estimates)))
    if len(workloads) < len(WORKLOADS):
        print("note: only %s ran; the score covers all of %s" % (", ".join(workloads), ", ".join(WORKLOADS)))
    if not full:
        print("note: the verifier runs scale 3; smaller scales build fewer levels, so these numbers differ")


if __name__ == "__main__":
    main()
