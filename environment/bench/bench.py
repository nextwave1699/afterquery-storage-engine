#!/usr/bin/env python3
"""Measure /app/leveldb against /opt/pristine the way the verifier does.

    bench.py [--seed N] [--scale X] [--candidate-only] [--keep]

Builds both engines (pristine only once) with lsmbench from this directory,
runs the workload on each, cross-reads the two databases and prints the
guardrails and an estimated score. Defaults to seed 1 at scale 3, which is
the verifier's scale.

The verifier scores a fixed seed of its own. The pristine engine's numbers on
that seed are in scored_baseline.json; a candidate's write amplification
barely moves with the seed, so candidate WA / that pristine WA is a good
estimate of the score. The ratio against seed 1's pristine is not (seed 1's
pristine writes more). --candidate-only skips the pristine build and checks
against scored_baseline.json alone (no RSS check).

It doesn't run the crash or compatibility checks, so passing here isn't a
guarantee.
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

MAX_L0_DEPTH = 4
SPACE_RATIO_MAX = 1.25
READ_RATIO_MAX = 1.5
FILES_RATIO_MAX = 3.0
MAX_FILE_SIZE = 8 * 1024 * 1024
RSS_EXTRA_MAX_KB = 96 * 1024
MEMTABLE_SWITCH_RATIO_MIN = 0.8
DISK_RATIO_MAX = 1.5


def scored_baseline():
    with open(os.path.join(HERE, "scored_baseline.json")) as f:
        return json.load(f)


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
    c = results["candidate"]
    ic = c["info"]
    b = results.get("pristine")
    if b is not None:
        print("cross reads ...")
        check(ref, results["candidate"]["db"], args.seed, args.scale, desc["batches"])
        check(cand, b["db"], args.seed, args.scale, desc["batches"])
        print("cross reads ok")

    # Guardrails are ratios to the pristine engine. Its numbers move with the
    # seed more than a good candidate's do, so compare against both the local
    # pristine run and the scored seed's pristine, and report the worse one.
    full = abs(args.scale - 3.0) < 1e-9
    sb = scored_baseline() if full else None

    def worst(cand_val, key_local, key_scored, higher_is_worse=True):
        ratios = []
        if b is not None:
            ratios.append(cand_val / max(1, key_local(b)))
        if sb is not None:
            ratios.append(cand_val / max(1, sb[key_scored]))
        if not ratios:
            return None
        return max(ratios) if higher_is_worse else min(ratios)

    checks = [("level-0 depth <= %d" % MAX_L0_DEPTH, ic["max_l0_depth"], ic["max_l0_depth"] <= MAX_L0_DEPTH, "%d"),
              ("largest table <= %d MB" % (MAX_FILE_SIZE >> 20), ic["max_file_size"] / 2**20,
               ic["max_file_size"] <= MAX_FILE_SIZE, "%.1f MB")]
    for name, val, limit, fmt in (
            ("peak space <= %.2fx" % SPACE_RATIO_MAX,
             worst(ic["max_total_bytes"], lambda r: r["info"]["max_total_bytes"], "max_total_bytes"), SPACE_RATIO_MAX, "%.3fx"),
            ("disk after run <= %.1fx" % DISK_RATIO_MAX,
             worst(ic["table_bytes_present"], lambda r: r["info"]["table_bytes_present"], "table_bytes_present"), DISK_RATIO_MAX, "%.3fx"),
            ("phase-F read bytes <= %.1fx" % READ_RATIO_MAX,
             worst(c["read_f"], lambda r: r["read_f"], "read_bytes_f"), READ_RATIO_MAX, "%.3fx"),
            ("peak file count <= %.1fx" % FILES_RATIO_MAX,
             worst(ic["max_files"], lambda r: r["info"]["max_files"], "max_files"), FILES_RATIO_MAX, "%.3fx")):
        if val is not None:
            checks.append((name, val, val <= limit, fmt))
    val = worst(ic["memtable_switches"], lambda r: r["info"]["memtable_switches"], "memtable_switches", False)
    if val is not None:
        checks.append(("memtable switches >= %.1fx" % MEMTABLE_SWITCH_RATIO_MIN, val,
                       val >= MEMTABLE_SWITCH_RATIO_MIN, "%.3fx"))
    if b is not None:
        extra = (c["rss_kb"] - b["rss_kb"]) // 1024
        checks.append(("peak RSS <= baseline + %d MB" % (RSS_EXTRA_MAX_KB >> 10), extra,
                       c["rss_kb"] - b["rss_kb"] <= RSS_EXTRA_MAX_KB, "+%d MB"))
    print()
    for name, val, ok, fmt in checks:
        print("  %-36s %-8s %s" % (name, "ok" if ok else "FAIL", fmt % val))
    print("\nguardrails: %s" % ("ok" if all(ok for _, _, ok, _ in checks) else "FAIL (score would not count)"))
    if sb is not None:
        print("estimated score: %.4f  (your WA %.3f / pristine WA %.4f on the scored seed; lower is better, untouched = 1.0)" % (
            c["wa"] / sb["wa"], c["wa"], sb["wa"]))
    if b is not None:
        print("ratio on seed %d: %.4f  (vs pristine WA %.3f on this seed; only the estimate above predicts the verifier)" % (
            args.seed, c["wa"] / b["wa"], b["wa"]))
    if not full:
        print("note: the verifier runs scale 3; smaller scales build fewer levels, so these numbers differ")
    if not args.keep:
        for r in results.values():
            shutil.rmtree(r["db"], ignore_errors=True)


if __name__ == "__main__":
    main()
