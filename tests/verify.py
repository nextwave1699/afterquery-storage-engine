#!/usr/bin/env python3
"""Verifier driver for the storage-engine task: range deletes and merge
operators for LevelDB 1.23.

Runs as root; anything that builds or links candidate code runs as the
unprivileged `lsm` user. Stages, each skipped if one it needs failed:

  build         pristine + candidate engines; our own lsmbench against each,
                our own conftest against the candidate (full extension API)
                and against the pristine (stock scenarios only)
  functional    lsmbench selftest on the candidate
  api           fixed checks of the extension API (conftest api)
  conformance   CONF_SCENARIOS seeded scenarios with range deletes and merge
                operands, CONF_LONG long ones, STOCK_SCENARIOS without them;
                every read checked against a model
  differential  scale 0.25 stock workload, each engine reads what the other
                wrote
  crash         SIGKILL at eleven Env events in a stock workload, both
                engines must recover the same batches (CRASH_SCENARIOS)
  recovery      RECOVERY_CASES seeded crashes at random Env events, most in
                scenarios with the extensions
  compat        sealed pristine-written dbs + one fresh one: candidate reads
                and extends, pristine reads the result
  efficiency    the rangedel and merge cases (conftest perf), kernel I/O
                counters of each phase against PERF_LIMITS
  benchmark     stock workloads (WORKLOADS) at full scale on both engines:
                write amplification at most WA_RATIO_MAX x the pristine's,
                plus the guardrails, so the stock paths do not regress

The gate is every stage.  The objective, feature_score, is the mean over
api, conformance, recovery and efficiency of the fraction of their checks
that passed (0 for the untouched tree, which has no extension API; 1 when
all pass).

The report (--out) is rewritten after every stage. test.sh turns it into
the reward files via test_outputs.py.
"""
import argparse
import json
import math
import os
import pwd
import random
import shutil
import signal
import subprocess
import sys
import tarfile
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "harness"))
import manifest  # noqa: E402


# The regression benchmark always uses this seed, so repeat runs measure the
# same bytes. It lives only here; bench.py defaults to seed 1.
BENCH_SEED = 604729
# Stock workloads the candidate must not regress on, and the pristine WA each
# should land in; outside the band something is off with the box.
WORKLOADS = ["mixed", "ttl", "bimodal"]
WA_RATIO_MAX = 1.10            # candidate WA vs pristine WA, per workload
BASELINE_WA_BAND = {
    "mixed": (9.9, 19.7), "uniform": (7.3, 14.6), "series": (3.0, 6.1), "blob": (3.6, 7.2),
    "hotkey": (2.5, 5.0), "bursts": (10.8, 21.6), "ttl": (2.6, 5.2), "scan": (5.6, 11.2),
    "bimodal": (3.6, 7.1), "rolling": (5.1, 10.1), "smallval": (6.8, 13.6), "wide": (6.7, 13.4)}
BENCH_SCALE = 3.0
SMALL_SCALE = 0.25
CRASH_SCALE = 0.3
FIXTURE_SCALE = 0.15
MAX_L0_DEPTH = 4               # level-0 files overlapping one key, at any version
SPACE_RATIO_MAX = 1.25         # peak table bytes vs. pristine
READ_RATIO_MAX = 1.5           # phase-F bytes read vs. pristine
# ttl keeps only a tenth of its ids live, so phase-F scans cross long runs of
# tombstones and read cost swings much harder there than elsewhere.
READ_RATIO_MAX_BY_WORKLOAD = {"ttl": 2.5}
FILES_RATIO_MAX = 3.0          # peak file count vs. pristine
MAX_FILE_SIZE = 8 * 1024 * 1024
RSS_EXTRA_MAX_KB = 96 * 1024   # peak RSS vs. pristine
# LevelDB can leave unreferenced tables around until the next compaction or
# open, so allow some, just not a pile.
ORPHAN_RATIO_MAX = 0.5
ORPHAN_SLACK = 16 * 1024 * 1024
DISK_RATIO_MAX = 1.5           # table bytes on disk after the run vs. pristine
MEMTABLE_SWITCH_RATIO_MIN = 0.8
TIME_RATIO_MAX = 4.0           # wall time vs. pristine (plus 60 s)
BUILD_TIMEOUT = int(os.environ.get("LSM_BUILD_TIMEOUT", "900"))
RUN_TIMEOUT = int(os.environ.get("LSM_RUN_TIMEOUT", "900"))
SMALL_TIMEOUT = int(os.environ.get("LSM_SMALL_TIMEOUT", "300"))


# The conformance and recovery suite (conftest).  Scenario seeds and crash
# points are fixed here, in the sealed verifier.
CONF_BASE = 910000
CONF_SCENARIOS = 12000         # with range deletes and merges, CONF_OPS operations each
CONF_OPS = 300
CONF_LONG_BASE = 970000
CONF_LONG = 400                # the same, CONF_LONG_OPS operations each
CONF_LONG_OPS = 3000
STOCK_BASE = 990000
STOCK_SCENARIOS = 1000         # stock API only
CONF_JOBS = 8
# Wall-clock budgets per stage, so a hanging candidate cannot keep the
# verifier busy for hours: whatever has not finished by then counts as failed.
CONF_DEADLINE = 1200
RECOVERY_DEADLINE = 1200
RECOVERY_CASE_TIMEOUT = 120
RECOVERY_SEED = 520000
RECOVERY_CASES = 400
API_CHECKS = 28                # conftest api
# Efficiency cases (conftest perf, scale 1).  A limit is either bytes or
# (factor, slack): at most factor * LOAD_TABLE_BYTES + slack.
PERF_LIMITS = {
    "rangedel": {"A_WCHAR": 512 << 10, "A_RCHAR": 512 << 10, "S_RCHAR": (0.5, 0),
                 "R_TABLE_BYTES": (0.3, 1 << 20), "B_TABLE_BYTES": (0.15, 1 << 20),
                 "VMHWM_KB": 1 << 20},
    "manyranges": {"M_CPU_MS": 15000, "VMHWM_KB": 1 << 20},
    "merge": {"A_WCHAR": 150 << 20, "A_RCHAR": 256 << 20, "B_TABLE_BYTES": (1.25, 0),
              "VMHWM_KB": 1 << 20},
    "cfwal": {"W_LOG_BYTES": 12 << 20, "W_WCHAR": 400 << 20, "VMHWM_KB": 1 << 20},
}
PERF_TIMEOUT = 300
STAGES = ("build", "functional", "api", "conformance", "differential", "crash", "recovery", "compat",
          "efficiency", "benchmark")
SCORED = ("api", "conformance", "recovery", "efficiency")


class Fail(Exception):
    pass


class Verifier:
    def __init__(self, args):
        self.args = args
        self.tree = args.tree
        self.sealed = args.sealed
        self.work = args.work
        self.user = args.user or None
        self.report = {"stages": {}, "log": [], "contract": {
            "bench_seed": BENCH_SEED, "bench_scale": BENCH_SCALE, "workloads": WORKLOADS,
            "wa_ratio_max": WA_RATIO_MAX, "conf_scenarios": CONF_SCENARIOS, "conf_long": CONF_LONG,
            "stock_scenarios": STOCK_SCENARIOS, "recovery_cases": RECOVERY_CASES,
            "perf_limits": PERF_LIMITS,
            "read_ratio_max_by_workload": READ_RATIO_MAX_BY_WORKLOAD, "max_l0_depth": MAX_L0_DEPTH,
            "space_ratio_max": SPACE_RATIO_MAX, "read_ratio_max": READ_RATIO_MAX,
            "files_ratio_max": FILES_RATIO_MAX, "max_file_size": MAX_FILE_SIZE,
            "rss_extra_max_kb": RSS_EXTRA_MAX_KB, "memtable_switch_ratio_min": MEMTABLE_SWITCH_RATIO_MIN,
            "time_ratio_max": TIME_RATIO_MAX, "disk_ratio_max": DISK_RATIO_MAX}}
        self.uid = self.gid = None
        if self.user:
            pw = pwd.getpwnam(self.user)
            self.uid, self.gid = pw.pw_uid, pw.pw_gid
        self.ref_bin = None
        self.cand_bin = None
        self.conf_bin = {}
        self.conf_error = None
        rng = random.SystemRandom()
        self.seed = args.seed if args.seed else rng.randint(1000, 999999)
        self.small_seed = self.seed + 7
        self.report["seed"] = self.seed
        self.t0 = time.time()


    def log(self, msg):
        line = "[%6.1fs] %s" % (time.time() - self.t0, msg)
        print(line, flush=True)
        self.report["log"].append(line)

    def save(self):
        tmp = self.args.out + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.report, f, indent=1, default=str)
        os.replace(tmp, self.args.out)

    def stage(self, name):
        return _Stage(self, name)

    def stage_ok(self, name):
        return self.report["stages"].get(name, {}).get("ok") is True

    def own(self, path):
        """chown -R to the unprivileged user."""
        if self.uid is None:
            return
        for root, dirs, files in os.walk(path):
            os.chown(root, self.uid, self.gid)
            for n in files:
                p = os.path.join(root, n)
                if not os.path.islink(p):
                    os.chown(p, self.uid, self.gid)
        os.chown(path, self.uid, self.gid)

    def udir(self, name):
        p = os.path.join(self.work, name)
        if os.path.exists(p):
            shutil.rmtree(p)
        os.makedirs(p)
        self.own(p)
        return p

    def kill_user_procs(self):
        if self.uid is None:
            return
        subprocess.run(["pkill", "-9", "-u", self.user], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)

    def run_as_user(self, cmd, timeout, cwd=None, log_name=None, as_root=False):
        """Returns (status, stdout, stderr). status is "timeout" on timeout."""
        kw = {}
        if self.uid is not None and not as_root:
            kw = {"user": self.uid, "group": self.gid}
        env = {"PATH": "/usr/local/bin:/usr/bin:/bin", "HOME": "/nonexistent",
               "TMPDIR": "/nonexistent", "LANG": "C"}
        p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, start_new_session=True, **kw)
        try:
            out, err = p.communicate(timeout=timeout)
            status = p.returncode
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            out, err = p.communicate()
            status = "timeout"
        out = out.decode("utf-8", "replace")
        err = err.decode("utf-8", "replace")
        if log_name:
            with open(os.path.join(self.work, "logs", log_name), "w") as f:
                f.write("$ %s\nstatus=%s\n--- stdout\n%s\n--- stderr\n%s\n" % (" ".join(cmd), status, out, err))
        return status, out, err

    def measured(self, name, cmd, timeout):
        """Run cmd under measure.py and return its JSON plus the output."""
        out_json = os.path.join(self.work, "meas", name + ".json")
        if os.path.exists(out_json):
            os.remove(out_json)
        status, out, err = self.run_as_user(
            ["python3", self.args.measure, out_json, str(timeout), "--"] + cmd,
            timeout=timeout + 60, log_name=name + ".log")
        self.kill_user_procs()
        m = {}
        if os.path.exists(out_json):
            try:
                with open(out_json) as f:
                    m = json.load(f)
            except Exception as e:
                m = {"error": "unreadable measurement: %s" % e}
        else:
            m = {"error": "no measurement (helper status %s): %s" % (status, err[-500:])}
        m["stdout"] = out
        m["stderr_tail"] = err[-2000:]
        m["name"] = name
        return m

    def harness(self, which, mode, db, seed, scale, extra=(), timeout=SMALL_TIMEOUT, name=None):
        binary = self.ref_bin if which == "ref" else self.cand_bin
        cmd = [binary, mode, "--db", db, "--seed", str(seed), "--scale", str(scale), "--quiet"] + list(extra)
        name = name or ("%s-%s-%s" % (which, mode, os.path.basename(db)))
        m = self.measured(name, cmd, timeout)
        return m

    @staticmethod
    def ok_run(m):
        return m.get("stopped") is True and not m.get("timed_out")

    @staticmethod
    def parse_applied(m):
        applied = final = None
        for line in m.get("stdout", "").splitlines():
            if line.startswith("APPLIED "):
                applied = int(line.split()[1])
            elif line.startswith("FINAL_BATCHES "):
                final = int(line.split()[1])
        return applied, final

    def expect_ok(self, m, what):
        if not self.ok_run(m):
            raise Fail("%s failed: %s" % (what, describe_failure(m)))

    def check(self, which, db, seed, scale, batches, cont=0, paranoid=True, name=None, timeout=SMALL_TIMEOUT,
              workload="mixed"):
        extra = ["--batches", str(batches), "--workload", workload]
        if cont:
            extra += ["--continue", str(cont)]
        if paranoid:
            extra.append("--paranoid")
        m = self.harness(which, "check", db, seed, scale, extra, timeout=timeout, name=name)
        self.expect_ok(m, "%s check of %s" % (which, os.path.basename(db)))
        applied, final = self.parse_applied(m)
        if applied is None:
            raise Fail("%s check of %s printed no APPLIED line" % (which, db))
        return applied, final, m

    def read_ack(self, path):
        with open(path) as f:
            return int(f.read().strip() or "0")

    def copy_db(self, src, name):
        dst = os.path.join(self.work, name)
        if os.path.exists(dst):
            shutil.rmtree(dst)
        shutil.copytree(src, dst)
        self.own(dst)
        return dst

    def hygiene(self, db, what):
        """Fail if the MANIFEST points at missing/resized tables or the
        directory is littered with orphans or old WALs."""
        info = manifest.summarize(db)
        if info["missing_tables"]:
            raise Fail("%s: manifest references missing tables %s" % (what, info["missing_tables"][:5]))
        if info["referenced_bytes_mismatch"]:
            raise Fail("%s: table sizes on disk differ from the manifest for %s" % (what, info["referenced_bytes_mismatch"][:5]))
        if info["orphan_bytes"] > ORPHAN_RATIO_MAX * info["referenced_bytes"] + ORPHAN_SLACK:
            raise Fail("%s: %d unreferenced tables (%d MB) left behind against %d MB referenced" % (
                what, len(info["orphan_tables"]), info["orphan_bytes"] >> 20, info["referenced_bytes"] >> 20))
        if len(info["stale_logs"]) > 1:
            raise Fail("%s: obsolete WAL files left behind: %s" % (what, info["stale_logs"][:5]))
        return info

    def describe(self, seed, scale, workload="mixed"):
        status, out, err = self.run_as_user([self.ref_bin, "describe", "--seed", str(seed), "--scale", str(scale),
                                             "--workload", workload], timeout=300)
        if status != 0:
            raise Fail("describe failed: %s" % err[-300:])
        return json.loads(out.strip().splitlines()[-1])


    def build(self):
        st = self.report["stages"]["build"] = {"ok": None}
        os.makedirs(os.path.join(self.work, "logs"), exist_ok=True)
        os.makedirs(os.path.join(self.work, "meas"), exist_ok=True)
        self.own(os.path.join(self.work, "meas"))
        pristine = os.path.join(self.work, "pristine")
        unpack = os.path.join(self.work, "unpack")
        for d in (pristine, unpack):
            if os.path.exists(d):
                shutil.rmtree(d)
        os.makedirs(unpack)
        with tarfile.open(os.path.join(self.sealed, "leveldb.tar.gz")) as tf:
            tf.extractall(unpack)
        os.rename(os.path.join(unpack, "leveldb"), pristine)
        if not os.path.isfile(os.path.join(pristine, "db", "version_set.cc")):
            raise Fail("sealed tree incomplete")
        self.log("building pristine engine")
        self.ref_bin = self.build_engine(pristine, os.path.join(self.work, "build-ref"), "ref", as_root=True)
        st["pristine_build_ok"] = True
        # Build the candidate from a copy owned by lsm.
        if not os.path.isfile(os.path.join(self.tree, "db", "version_set.cc")):
            raise Fail("candidate tree %s is missing db/version_set.cc" % self.tree)
        cand = os.path.join(self.work, "cand")
        if os.path.exists(cand):
            shutil.rmtree(cand)
        shutil.copytree(self.tree, cand, symlinks=True,
                        ignore=shutil.ignore_patterns(".git", "build", "__pycache__"))
        self.own(cand)
        self.log("building candidate engine")
        self.cand_bin = self.build_engine(cand, os.path.join(self.work, "build-cand"), "cand", as_root=False)
        st["candidate_build_ok"] = True
        st["ok"] = True

    def build_engine(self, tree, build_dir, tag, as_root):
        if os.path.exists(build_dir):
            shutil.rmtree(build_dir)
        os.makedirs(build_dir)
        if not as_root:
            self.own(build_dir)
        cmake = ["cmake", "-DCMAKE_BUILD_TYPE=Release", "-DLEVELDB_BUILD_TESTS=OFF",
                 "-DLEVELDB_BUILD_BENCHMARKS=OFF", "-DLEVELDB_INSTALL=OFF", tree]
        status, out, err = self.run_as_user(cmake, BUILD_TIMEOUT, cwd=build_dir,
                                            log_name="cmake-%s.log" % tag, as_root=as_root)
        if status != 0:
            raise Fail("cmake failed for %s (status %s): %s" % (tag, status, (out + err)[-1500:]))
        status, out, err = self.run_as_user(["make", "-j4", "leveldb"], BUILD_TIMEOUT, cwd=build_dir,
                                            log_name="make-%s.log" % tag, as_root=as_root)
        lib = os.path.join(build_dir, "libleveldb.a")
        if status != 0 or not os.path.isfile(lib):
            raise Fail("build failed for %s (status %s): %s" % (tag, status, (out + err)[-1500:]))
        self.kill_user_procs()
        # Always our copy of the harness, never the one in /app/bench.
        binary = os.path.join(self.work, "lsmbench-" + tag)
        hs = os.path.join(HERE, "harness")
        cc = ["g++", "-std=c++17", "-O2", "-fno-rtti", "-I" + os.path.join(tree, "include"), "-I" + hs,
              os.path.join(hs, "lsmbench.cc"), "-o", binary, lib, "-lpthread"]
        status, out, err = self.run_as_user(cc, BUILD_TIMEOUT, log_name="harness-%s.log" % tag, as_root=True)
        if status != 0 or not os.path.isfile(binary):
            raise Fail("harness does not compile against the %s tree (status %s): %s" % (tag, status, err[-1500:]))
        os.chmod(binary, 0o755)
        # the conformance and recovery suite, same rules; the pristine tree
        # has no extension API, so it gets the stock-only build
        conf = os.path.join(self.work, "conftest-" + tag)
        cc = ["g++", "-std=c++17", "-O2", "-fno-rtti", "-I" + os.path.join(tree, "include"), "-I" + hs,
              os.path.join(hs, "conftest.cc"), os.path.join(hs, "conf_model.cc"), "-o", conf, lib, "-lpthread"]
        if tag == "ref":
            cc.insert(1, "-DCONF_PRISTINE")
        status, out, err = self.run_as_user(cc, BUILD_TIMEOUT, log_name="conftest-%s.log" % tag, as_root=True)
        if status != 0 or not os.path.isfile(conf):
            if tag == "ref":
                raise Fail("conformance suite does not compile against the pristine tree: %s" % err[-1500:])
            # The engine builds but does not offer the extension API (or
            # offers it with other signatures).  Stages that need it fail.
            self.conf_error = "the conformance suite does not compile against the candidate's include/: %s" % (
                err[-2500:])
            self.report["stages"]["build"]["conftest_error"] = self.conf_error
            self.log("conftest does not compile against the candidate")
            return binary
        os.chmod(conf, 0o755)
        self.conf_bin[tag] = conf
        return binary

    def functional(self):
        st = self.report["stages"]["functional"] = {"ok": None}
        db = self.udir("db-selftest")
        m = self.harness("cand", "selftest", db, 1, 1, name="cand-selftest")
        self.expect_ok(m, "selftest")
        st["ok"] = True

    def need_conftest(self):
        if "cand" not in self.conf_bin:
            raise Fail(self.conf_error or "conformance suite unavailable")

    def api(self):
        st = self.report["stages"]["api"] = {"ok": None, "passed": 0, "total": 0}
        self.need_conftest()
        db = self.udir("api")
        status, out, err = self.run_as_user([self.conf_bin["cand"], "api", "--db", db], timeout=SMALL_TIMEOUT,
                                            log_name="api.log")
        self.kill_user_procs()
        lines = [l for l in out.splitlines() if l.startswith("API ")]
        st["checks"] = lines
        st["passed"] = sum(1 for l in lines if l.split()[2:3] == ["ok"])
        st["total"] = max(len(lines), API_CHECKS)
        if status != 0 or st["passed"] != st["total"]:
            bad = [l for l in lines if " FAIL" in l]
            raise Fail("API checks: %d of %d passed (status %s): %s" % (
                st["passed"], st["total"], status, "; ".join(bad[:4]) or (out + err)[-600:]))
        st["ok"] = True

    def run_scenarios(self, name, lo, hi, ops, ext, deadline, jobs=CONF_JOBS):
        """Runs seeds [lo, hi) in `jobs` processes; returns (passed, failures)."""
        step = (hi - lo + jobs - 1) // jobs
        procs = []
        for i in range(jobs):
            a, b = lo + i * step, min(hi, lo + (i + 1) * step)
            if a >= b:
                continue
            db = self.udir("%s-%d" % (name, i))
            cmd = [self.conf_bin["cand"], "scenarios", "--db", db, "--from", str(a), "--to", str(b),
                   "--ops", str(ops), "--quiet"] + (["--ext"] if ext else [])
            procs.append((self.spawn_as_user(cmd, "%s-%d.log" % (name, i)), b - a))
        passed = 0
        failures = []
        for (p, log), n in procs:
            status, out = self.wait_spawned(p, log, max(1, deadline - time.time()))
            got = [l for l in out.splitlines() if l.startswith("PASSED ")]
            fails = [l for l in out.splitlines() if " FAIL " in l]
            failures += fails
            if got:
                passed += int(got[-1].split()[1].split("/")[0])
            else:
                # the chunk died (crash, timeout): count what finished before
                done = [l for l in out.splitlines() if l.startswith("SCENARIO ")]
                passed += sum(1 for l in done if l.endswith(" ok"))
                failures.append("%s chunk of %d scenarios died (status %s) after %d: %s" % (
                    name, n, status, len(done), out[-300:].replace("\n", " | ")))
        self.kill_user_procs()
        return passed, failures

    def conformance(self):
        """Every scenario seed must pass on the candidate."""
        st = self.report["stages"]["conformance"] = {"ok": None, "passed": 0,
                                                     "total": CONF_SCENARIOS + CONF_LONG + STOCK_SCENARIOS}
        self.need_conftest()
        results = {}
        deadline = time.time() + CONF_DEADLINE
        for name, lo, n, ops, ext in (("conf", CONF_BASE, CONF_SCENARIOS, CONF_OPS, True),
                                      ("long", CONF_LONG_BASE, CONF_LONG, CONF_LONG_OPS, True),
                                      ("stock", STOCK_BASE, STOCK_SCENARIOS, CONF_OPS, False)):
            passed, failures = self.run_scenarios(name, lo, lo + n, ops, ext, deadline)
            results[name] = {"passed": passed, "total": n, "failures": failures[:10]}
            st["passed"] += passed
            self.log("conformance %s: %d/%d scenarios" % (name, passed, n))
            self.save()
        st["sets"] = results
        failures = [f for r in results.values() for f in r["failures"]]
        if st["passed"] != st["total"]:
            raise Fail("conformance: %d of %d scenarios passed; first failures: %s" % (
                st["passed"], st["total"], "; ".join(failures[:3])))
        st["ok"] = True

    def recovery(self):
        """Crash scenarios at random Env events.  Whatever the candidate left
        behind must be recovered by the candidate to an acknowledged state;
        in stock scenarios the pristine engine must recover the same one, and
        the candidate must recover what a crashed pristine left."""
        st = self.report["stages"]["recovery"] = {"ok": None, "cases": [], "passed": 0, "total": RECOVERY_CASES}
        self.need_conftest()
        rng = random.Random(RECOVERY_SEED)
        events = ["log_append", "table_append", "table_close", "table_sync", "manifest_append",
                  "manifest_sync", "remove_file", "rename"]
        crashed = 0
        failures = []
        deadline = time.time() + RECOVERY_DEADLINE
        for i in range(RECOVERY_CASES):
            seed = RECOVERY_SEED + i
            ev = rng.choice(events)
            n = 1 + rng.randrange(3 if ev in ("rename", "manifest_sync") else 60)
            spec = "%s:%d%s" % (ev, n, ":after" if rng.random() < 0.3 else "")
            kind = ("ext", "ext", "ext", "stock", "pristine")[i % 5]
            writer = "ref" if kind == "pristine" else "cand"
            ext = ["--ext"] if kind == "ext" else []
            db = self.udir("rec-%d" % i)
            ack = os.path.join(self.udir("rec-ack-%d" % i), "ack")
            case = {"spec": spec, "kind": kind}
            left = deadline - time.time()
            if left <= 0:
                failures.append("case %d: the stage ran out of time (%ds)" % (i, RECOVERY_DEADLINE))
                case["ok"] = False
                st["cases"].append(case)
                continue
            case_timeout = int(max(5, min(RECOVERY_CASE_TIMEOUT, left)))
            try:
                status, out, err = self.run_as_user(
                    [self.conf_bin[writer], "crash", "--db", db, "--seed", str(seed), "--crash", spec,
                     "--ack", ack, "--ops", str(CONF_OPS)] + ext, timeout=case_timeout)
                self.kill_user_procs()
                if status not in (0, -9):
                    raise Fail("the run failed before crashing: %s" % (out + err)[-400:])
                crashed += status == -9
                acked = self.read_ack(ack)
                case["acked"] = acked
                got = {}
                readers = ("cand", "ref") if kind == "stock" else ("cand",)
                for reader in readers:
                    copy = self.copy_db(db, "rec-%d-%s" % (i, reader))
                    status, out, err = self.run_as_user(
                        [self.conf_bin[reader], "recover", "--db", copy, "--seed", str(seed),
                         "--acked", str(acked), "--ops", str(CONF_OPS)] + ext, timeout=case_timeout)
                    self.kill_user_procs()
                    line = [l for l in out.splitlines() if l.startswith("RECOVERED ")]
                    if status != 0 or not line:
                        raise Fail("%s recovery failed (%d writes acked): %s" % (reader, acked, (out + err)[-500:]))
                    got[reader] = int(line[0].split()[1])
                    shutil.rmtree(copy, ignore_errors=True)
                case["recovered"] = got
                if len(set(got.values())) != 1:
                    raise Fail("the engines recovered different states %s" % got)
                case["ok"] = True
                st["passed"] += 1
            except Fail as e:
                case["ok"] = False
                case["error"] = str(e)
                failures.append("case %d (%s, %s): %s" % (i, spec, kind, e))
            st["cases"].append(case)
            shutil.rmtree(db, ignore_errors=True)
        st["crashed"] = crashed
        self.log("recovery: %d/%d cases, %d crashed at their event" % (st["passed"], RECOVERY_CASES, crashed))
        if failures:
            raise Fail("recovery: %d of %d cases passed; first failures: %s" % (
                st["passed"], RECOVERY_CASES, "; ".join(failures[:3])))
        st["ok"] = True

    def efficiency(self):
        """The perf cases: each phase's kernel I/O counters within limits."""
        st = self.report["stages"]["efficiency"] = {"ok": None, "cases": {}, "passed": 0, "total": 0}
        self.need_conftest()
        failures = []
        for case, limits in PERF_LIMITS.items():
            st["total"] += len(limits) + 1
            r = st["cases"][case] = {}
            db = self.udir("perf-" + case)
            t = time.time()
            status, out, err = self.run_as_user([self.conf_bin["cand"], "perf", "--db", db, "--case", case],
                                                timeout=PERF_TIMEOUT, log_name="perf-%s.log" % case)
            self.kill_user_procs()
            r["wall_s"] = time.time() - t
            metrics = {}
            for line in out.splitlines():
                parts = line.split()
                if len(parts) == 3 and parts[0] == "METRIC":
                    metrics[parts[1]] = int(parts[2])
            r["metrics"] = metrics
            if status != 0 or "PERF ok" not in out:
                failures.append("%s: the case failed (status %s): %s" % (case, status, (out + err)[-400:]))
                continue
            st["passed"] += 1
            checks = r["checks"] = {}
            for key, limit in limits.items():
                if isinstance(limit, tuple):
                    limit = int(limit[0] * metrics.get("LOAD_TABLE_BYTES", 0) + limit[1])
                val = metrics.get(key)
                ok = val is not None and val <= limit
                checks[key] = {"value": val, "limit": limit, "ok": ok}
                if ok:
                    st["passed"] += 1
                else:
                    failures.append("%s: %s = %s, limit %d" % (case, key, val, limit))
            self.log("efficiency %s: %s" % (case, ", ".join("%s %s/%s" % (k, c["value"], c["limit"])
                                                            for k, c in checks.items())))
        if failures:
            raise Fail("efficiency: " + "; ".join(failures))
        st["ok"] = True

    def spawn_as_user(self, cmd, log_name):
        kw = {}
        if self.uid is not None:
            kw = {"user": self.uid, "group": self.gid}
        env = {"PATH": "/usr/local/bin:/usr/bin:/bin", "HOME": "/nonexistent", "TMPDIR": "/nonexistent", "LANG": "C"}
        p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             start_new_session=True, **kw)
        return p, log_name

    def wait_spawned(self, p, log_name, timeout):
        try:
            out, _ = p.communicate(timeout=timeout)
            status = p.returncode
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            out, _ = p.communicate()
            status = "timeout"
        out = out.decode("utf-8", "replace")
        with open(os.path.join(self.work, "logs", log_name), "w") as f:
            f.write("status=%s\n%s" % (status, out))
        return status, out

    def differential(self):
        st = self.report["stages"]["differential"] = {"ok": None}
        seed, scale = self.small_seed, SMALL_SCALE
        desc = self.describe(seed, scale)
        st["batches"] = desc["batches"]
        # candidate writes, both read
        db = self.udir("db-diff-cand")
        m = self.harness("cand", "run", db, seed, scale, name="diff-cand-run")
        self.expect_ok(m, "candidate small run")
        m = self.harness("cand", "read", db, seed, scale, name="diff-cand-read")
        self.expect_ok(m, "candidate small read phase")
        a, _, _ = self.check("ref", self.copy_db(db, "db-diff-cand-copy"), seed, scale, desc["batches"], name="diff-ref-reads-cand")
        if a != desc["batches"]:
            raise Fail("pristine reader saw %d batches in the candidate's database, expected %d" % (a, desc["batches"]))
        a, _, _ = self.check("cand", db, seed, scale, desc["batches"], name="diff-cand-reads-cand")
        if a != desc["batches"]:
            raise Fail("candidate reader saw %d batches, expected %d" % (a, desc["batches"]))
        # pristine writes, candidate reads and extends, pristine reads
        db = self.udir("db-diff-ref")
        m = self.harness("ref", "run", db, seed, scale, name="diff-ref-run")
        self.expect_ok(m, "pristine small run")
        a, final, _ = self.check("cand", db, seed, scale, desc["batches"], cont=500, name="diff-cand-reads-ref")
        if a != desc["batches"] or final != desc["batches"] + 500:
            raise Fail("candidate could not read/extend the pristine database (applied %s, final %s)" % (a, final))
        a, _, _ = self.check("ref", db, seed, scale, final, name="diff-ref-reads-ext")
        if a != final:
            raise Fail("pristine reader disagrees after the candidate extended the database")
        st["ok"] = True

    CRASH_SCENARIOS = [
        # --arm-at-batch N starts counting once batch N is acked, then
        # EVENT:K kills before the K-th such event (":after" = just after it).
        # Batch numbers are spread out so each lands in a different cycle.
        ("wal-append", ["--arm-at-batch", "20000", "--crash", "log_append:40"]),
        ("table-close", ["--arm-at-batch", "20000", "--crash", "table_close:1"]),
        ("before-manifest", ["--arm-at-batch", "22000", "--crash", "manifest_append:1"]),
        ("after-manifest", ["--arm-at-batch", "24000", "--crash", "manifest_append:1:after"]),
        ("before-remove", ["--arm-at-batch", "26000", "--crash", "remove_file:3"]),
        ("mid-table", ["--arm-at-batch", "28000", "--crash", "table_append:200"]),
        ("mid-compaction", ["--arm-at-batch", "30000", "--crash", "table_create:3"]),
        ("compaction-installed", ["--arm-at-batch", "32000", "--crash", "manifest_append:2:after"]),
        ("reopen-current", ["--reopen-at", "25000", "--arm-at-reopen", "--crash", "rename:1"]),
        ("reopen-recovery", ["--reopen-at", "25000", "--arm-at-reopen", "--crash", "manifest_append:1:after"]),
        ("torn-wal", ["--arm-at-batch", "35000", "--crash", "op:1"]),
    ]

    def crash(self):
        st = self.report["stages"]["crash"] = {"ok": None, "scenarios": {}}
        seed, scale = self.small_seed + 1, CRASH_SCALE
        desc = self.describe(seed, scale)
        total_batches = desc["batches"]
        for name, extra in self.CRASH_SCENARIOS:
            sc = st["scenarios"][name] = {}
            db = self.udir("db-crash-" + name)
            ack = os.path.join(self.udir("ack-" + name), "ack")
            m = self.harness("cand", "run", db, seed, scale, extra + ["--ack", ack], name="crash-%s-run" % name)
            if m.get("stopped"):
                raise Fail("crash %s: the run completed without reaching the crash point (%s)" % (name, " ".join(extra)))
            if m.get("signal") != 9:
                raise Fail("crash %s: the process did not die at the trigger (%s)" % (name, describe_failure(m)))
            acked = self.read_ack(ack)
            sc["acked"] = acked
            if acked < 20000 or acked >= total_batches:
                raise Fail("crash %s: crashed after %d batches, outside the armed window" % (name, acked))
            expect = acked
            if name == "torn-wal":
                # Chop the end off the newest WAL. That should cost us the
                # last record and nothing else.
                logs = [n for n in os.listdir(db) if n.endswith(".log")]
                logs.sort(key=lambda n: int(n.split(".")[0]))
                if not logs:
                    raise Fail("torn-wal: no live WAL after the crash")
                path = os.path.join(db, logs[-1])
                size = os.path.getsize(path)
                if size < 64:
                    raise Fail("torn-wal: WAL too small (%d bytes) to cut" % size)
                with open(path, "r+b") as f:
                    f.truncate(size - 5)
                sc["truncated"] = logs[-1]
                expect = acked - 1
            # pristine recovers a copy of the crashed dir first
            copy = self.copy_db(db, "db-crash-%s-refcopy" % name)
            a_ref, _, _ = self.check("ref", copy, seed, scale, expect, name="crash-%s-ref" % name)
            shutil.rmtree(copy, ignore_errors=True)
            # then the candidate recovers the original and keeps writing
            a_cand, final, _ = self.check("cand", db, seed, scale, expect, cont=3000, name="crash-%s-cand" % name)
            sc["recovered"] = a_cand
            if a_cand != a_ref:
                raise Fail("crash %s: pristine recovered %d batches, candidate %d" % (name, a_ref, a_cand))
            if final is None or final < a_cand + 1:
                raise Fail("crash %s: candidate did not continue after recovery" % name)
            # and both can still open what it left behind
            a2, _, _ = self.check("ref", db, seed, scale, final, name="crash-%s-ref2" % name)
            if a2 != final:
                raise Fail("crash %s: pristine reads %d batches after continuation, expected %d" % (name, a2, final))
            a3, _, _ = self.check("cand", db, seed, scale, final, name="crash-%s-cand2" % name)
            if a3 != final:
                raise Fail("crash %s: candidate reads %d batches after the pristine reopen, expected %d" % (name, a3, final))
            info = self.hygiene(db, "crash %s" % name)
            sc["layout"] = info["levels"]
            sc["orphans"] = len(info["orphan_tables"])
            sc["ok"] = True
            shutil.rmtree(db, ignore_errors=True)
        st["ok"] = True

    def compat(self):
        st = self.report["stages"]["compat"] = {"ok": None, "fixtures": {}}
        fx_dir = os.path.join(self.work, "fixtures")
        if os.path.exists(fx_dir):
            shutil.rmtree(fx_dir)
        os.makedirs(fx_dir)
        with tarfile.open(os.path.join(self.sealed, "fixtures.tar.gz")) as tf:
            tf.extractall(fx_dir)
        with open(os.path.join(fx_dir, "fixtures.json")) as f:
            fixtures = json.load(f)
        # plus one written right now with a seed nobody has seen
        fresh_seed = self.seed + 11
        fresh = os.path.join(fx_dir, "fresh")
        os.makedirs(fresh)
        self.own(fresh)
        m = self.harness("ref", "run", fresh, fresh_seed, FIXTURE_SCALE, name="compat-fresh-run")
        self.expect_ok(m, "fresh fixture generation")
        fixtures.append({"name": "fresh", "dir": "fresh", "seed": fresh_seed, "scale": FIXTURE_SCALE,
                         "batches": self.describe(fresh_seed, FIXTURE_SCALE)["batches"], "extra": []})
        for fx in fixtures:
            r = st["fixtures"][fx["name"]] = {}
            db = os.path.join(fx_dir, fx["dir"])
            if not os.path.isdir(db):
                raise Fail("fixture %s missing" % fx["name"])
            self.own(db)
            extra = list(fx.get("extra", []))
            # candidate reads and extends the pristine-written database
            a, final, _ = self.check("cand", db, fx["seed"], fx["scale"], fx["batches"], cont=1000,
                                     name="compat-%s-cand" % fx["name"])
            r["applied"] = a
            allowed = {fx["batches"], fx["batches"] + 1}
            if a not in allowed:
                raise Fail("fixture %s: candidate read %s batches, expected one of %s" % (fx["name"], a, sorted(allowed)))
            if final is None or final < a + 1:
                raise Fail("fixture %s: candidate did not extend the database" % fx["name"])
            # pristine reads what the candidate left behind
            a2, _, _ = self.check("ref", db, fx["seed"], fx["scale"], final, name="compat-%s-ref" % fx["name"])
            if a2 != final:
                raise Fail("fixture %s: pristine reads %d batches, expected %d" % (fx["name"], a2, final))
            self.hygiene(db, "fixture %s" % fx["name"])
            r["ok"] = True
        st["ok"] = True

    def benchmark(self):
        st = self.report["stages"]["benchmark"] = {"ok": None, "workloads": {}}
        fails = []
        for w in WORKLOADS:
            res = st["workloads"][w] = self.bench_workload(w)
            fails += ["%s: %s" % (w, f) for f in res["guardrails"]["failures"]]
            self.save()
        ws = st["workloads"]
        for key in ("baseline_wa", "candidate_wa", "wa_ratio"):
            st[key] = geomean([ws[w][key] for w in WORKLOADS])
        st["guardrails"] = {"ok": not fails, "failures": fails}
        st["ok"] = True
        if fails:
            self.log("guardrail failures: " + "; ".join(fails))
        self.log("write amplification vs pristine (geometric mean) %.4f: %s" % (
            st["wa_ratio"], ", ".join("%s %.4f" % (w, ws[w]["wa_ratio"]) for w in WORKLOADS)))

    def bench_workload(self, w):
        seed, scale = self.args.bench_seed or BENCH_SEED, BENCH_SCALE
        desc = self.describe(seed, scale, w)
        logical = desc["logical_bytes"]
        st = {"logical_bytes": logical, "batches": desc["batches"]}
        results = {}
        for which in ("ref", "cand"):
            db = self.udir("db-bench-%s-%s" % (w, which))
            r = results[which] = {}
            t = time.time()
            m = self.harness(which, "run", db, seed, scale,
                             ["--workload", w, "--stats", os.path.join(self.work, "meas", "stats-%s-%s-run.json" % (w, which))],
                             timeout=RUN_TIMEOUT, name="bench-%s-%s-run" % (w, which))
            self.expect_ok(m, "%s %s benchmark run" % (which, w))
            r["run"] = strip(m)
            info_run = self.hygiene(db, "%s %s benchmark run" % (which, w))
            r["manifest_run"] = {k: info_run[k] for k in ("levels", "level_bytes", "max_l0", "max_l0_depth",
                                                          "max_files", "max_file_size", "max_total_bytes",
                                                          "mean_total_bytes", "edits", "files_added", "bytes_added",
                                                          "memtable_switches", "table_bytes_present",
                                                          "orphan_bytes", "referenced_bytes")}
            m = self.harness(which, "read", db, seed, scale,
                             ["--workload", w, "--stats", os.path.join(self.work, "meas", "stats-%s-%s-read.json" % (w, which))],
                             timeout=RUN_TIMEOUT, name="bench-%s-%s-read" % (w, which))
            self.expect_ok(m, "%s %s benchmark read phase" % (which, w))
            r["read"] = strip(m)
            r["wall_s"] = time.time() - t
            r["written"] = r["run"]["wchar"] + r["read"]["wchar"]
            r["wa"] = r["written"] / float(logical)
            r["read_bytes_f"] = r["read"]["rchar"]
            r["maxrss_kb"] = max(r["run"]["maxrss_kb"], r["read"]["maxrss_kb"])
            self.log("%s %s: write amplification %.3f (%.0f MB written / %.0f MB logical), phase F read %.0f MB, rss %d MB, %.0fs" % (
                w, which, r["wa"], r["written"] / 1e6, logical / 1e6, r["read_bytes_f"] / 1e6, r["maxrss_kb"] // 1024, r["wall_s"]))
        # cross reads at full scale
        db_ref = os.path.join(self.work, "db-bench-%s-ref" % w)
        db_cand = os.path.join(self.work, "db-bench-%s-cand" % w)
        a, _, _ = self.check("ref", db_cand, seed, scale, desc["batches"], workload=w,
                             name="bench-%s-ref-reads-cand" % w, timeout=RUN_TIMEOUT)
        if a != desc["batches"]:
            raise Fail("%s: pristine reader saw %d batches in the candidate's benchmark database" % (w, a))
        a, _, _ = self.check("cand", db_ref, seed, scale, desc["batches"], workload=w,
                             name="bench-%s-cand-reads-ref" % w, timeout=RUN_TIMEOUT)
        if a != desc["batches"]:
            raise Fail("%s: candidate reader saw %d batches in the pristine benchmark database" % (w, a))
        shutil.rmtree(db_ref, ignore_errors=True)
        shutil.rmtree(db_cand, ignore_errors=True)
        st["results"] = results
        base, cand = results["ref"], results["cand"]
        st["baseline_wa"] = base["wa"]
        st["candidate_wa"] = cand["wa"]
        st["wa_ratio"] = cand["wa"] / base["wa"]
        band = BASELINE_WA_BAND[w]
        if not (band[0] <= base["wa"] <= band[1]):
            raise Fail("verifier self-check: pristine %s write amplification %.2f outside %s" % (w, base["wa"], band))
        g = st["guardrails"] = {}
        mr_b, mr_c = base["manifest_run"], cand["manifest_run"]
        g["max_l0_depth"] = mr_c["max_l0_depth"]
        g["space_ratio"] = mr_c["max_total_bytes"] / float(max(1, mr_b["max_total_bytes"]))
        g["read_ratio"] = cand["read_bytes_f"] / float(max(1, base["read_bytes_f"]))
        g["files_ratio"] = mr_c["max_files"] / float(max(1, mr_b["max_files"]))
        g["max_file_size"] = mr_c["max_file_size"]
        g["rss_extra_kb"] = cand["maxrss_kb"] - base["maxrss_kb"]
        g["memtable_switch_ratio"] = mr_c["memtable_switches"] / float(max(1, mr_b["memtable_switches"]))
        g["time_ratio"] = cand["wall_s"] / max(1.0, base["wall_s"])
        g["disk_ratio"] = mr_c["table_bytes_present"] / float(max(1, mr_b["table_bytes_present"]))
        fails = []
        if st["wa_ratio"] > WA_RATIO_MAX:
            fails.append("write amplification %.3fx the pristine's > %.2f" % (st["wa_ratio"], WA_RATIO_MAX))
        if g["disk_ratio"] > DISK_RATIO_MAX:
            fails.append("table bytes on disk after the run %.2fx baseline > %.2f" % (g["disk_ratio"], DISK_RATIO_MAX))
        if g["max_l0_depth"] > MAX_L0_DEPTH:
            fails.append("level-0 depth %d > %d" % (g["max_l0_depth"], MAX_L0_DEPTH))
        if g["space_ratio"] > SPACE_RATIO_MAX:
            fails.append("peak space %.2fx baseline > %.2f" % (g["space_ratio"], SPACE_RATIO_MAX))
        read_max = READ_RATIO_MAX_BY_WORKLOAD.get(w, READ_RATIO_MAX)
        if g["read_ratio"] > read_max:
            fails.append("phase-F read bytes %.2fx baseline > %.2f" % (g["read_ratio"], read_max))
        if g["files_ratio"] > FILES_RATIO_MAX:
            fails.append("peak file count %.2fx baseline > %.2f" % (g["files_ratio"], FILES_RATIO_MAX))
        if g["max_file_size"] > MAX_FILE_SIZE:
            fails.append("a table of %d bytes exceeds %d" % (g["max_file_size"], MAX_FILE_SIZE))
        if g["rss_extra_kb"] > RSS_EXTRA_MAX_KB:
            fails.append("peak RSS exceeds baseline by %d MB" % (g["rss_extra_kb"] // 1024))
        if g["memtable_switch_ratio"] < MEMTABLE_SWITCH_RATIO_MIN:
            fails.append("only %.2fx the baseline's memtable switches (write_buffer_size not honoured?)" % g["memtable_switch_ratio"])
        if cand["wall_s"] > TIME_RATIO_MAX * base["wall_s"] + 60:
            fails.append("candidate took %.0fs vs %.0fs baseline" % (cand["wall_s"], base["wall_s"]))
        g["failures"] = fails
        g["ok"] = not fails
        self.log("%s: candidate WA %.3f, pristine %.3f, ratio %.4f%s" % (
            w, cand["wa"], base["wa"], st["wa_ratio"], ("; " + "; ".join(fails)) if fails else ""))
        return st

    def run(self):
        self.save()
        with self.stage("build"):
            self.build()
        if self.stage_ok("build"):
            with self.stage("functional"):
                self.functional()
            with self.stage("api"):
                self.api()
            with self.stage("conformance"):
                self.conformance()
        if self.stage_ok("functional"):
            with self.stage("differential"):
                self.differential()
        if self.stage_ok("build"):
            with self.stage("recovery"):
                self.recovery()
        if self.stage_ok("differential"):
            with self.stage("crash"):
                self.crash()
            with self.stage("compat"):
                self.compat()
        if self.stage_ok("build"):
            with self.stage("efficiency"):
                self.efficiency()
        if all(self.stage_ok(n) for n in ("functional", "differential", "crash", "compat")):
            with self.stage("benchmark"):
                self.benchmark()
        s = self.report["stages"]
        gate = all(self.stage_ok(n) for n in STAGES)
        bench = s.get("benchmark", {})
        fractions = {}
        for n in SCORED:
            st = s.get(n, {})
            total = st.get("total") or 0
            fractions[n] = (st.get("passed", 0) / float(total)) if total else 0.0
        self.report["summary"] = {
            "gate_passed": gate,
            "reward": 1 if gate else 0,
            "feature_score": sum(fractions.values()) / len(SCORED),
            "fractions": fractions,
            "wa_ratio": bench.get("wa_ratio"),
            "elapsed_s": time.time() - self.t0,
        }
        self.save()
        self.log("summary: %s" % json.dumps(self.report["summary"]))
        return 0


class _Stage:
    def __init__(self, v, name):
        self.v, self.name = v, name

    def __enter__(self):
        self.v.log("stage %s" % self.name)
        self.v.report["stages"].setdefault(self.name, {"ok": None})
        return self

    def __exit__(self, et, ev, tb):
        st = self.v.report["stages"][self.name]
        if et is None:
            if st.get("ok") is None:
                st["ok"] = True
            self.v.log("stage %s: ok" % self.name)
        else:
            st["ok"] = False
            if isinstance(ev, Fail):
                st["error"] = str(ev)
            else:
                st["error"] = "verifier exception: %s" % "".join(traceback.format_exception(et, ev, tb))[-3000:]
            self.v.log("stage %s: FAILED: %s" % (self.name, st["error"][:500]))
        self.v.kill_user_procs()
        self.v.save()
        return True  # swallow it; the stage is marked failed


def geomean(values):
    return math.exp(sum(math.log(v) for v in values) / len(values))


def describe_failure(m):
    if "error" in m:
        return m["error"]
    if m.get("timed_out"):
        return "timed out after %.0fs" % m.get("wall_s", 0)
    if m.get("signal"):
        return "killed by signal %d; stderr: %s" % (m["signal"], m.get("stderr_tail", "")[-400:])
    return "exit status %s; stderr: %s" % (m.get("exit_status"), m.get("stderr_tail", "")[-400:])


def strip(m):
    keep = ("stopped", "timed_out", "exit_status", "signal", "wchar", "rchar", "write_bytes",
            "read_bytes", "maxrss_kb", "vmhwm_kb", "utime", "stime", "wall_s")
    return {k: m.get(k) for k in keep}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="/app/leveldb")
    ap.add_argument("--sealed", default=os.path.join(HERE, "sealed"))
    ap.add_argument("--work", default="/tmp/lsm-verify")
    ap.add_argument("--out", default="/tmp/lsm-verify-report.json")
    ap.add_argument("--user", default="lsm")
    ap.add_argument("--measure", default="/opt/verifier/measure.py")
    ap.add_argument("--seed", type=int, default=0, help="seed for the unscored stages (default: random)")
    ap.add_argument("--bench-seed", type=int, default=0, help="override BENCH_SEED (local experiments only)")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    v = Verifier(args)
    try:
        return v.run()
    except Exception:
        # bug in the driver itself; still leave a reward-0 report behind
        v.report["fatal"] = traceback.format_exc()
        v.report.setdefault("summary", {"reward": 0, "correctness_passed": False,
                                        "guardrails_passed": False})
        v.save()
        return 1


if __name__ == "__main__":
    sys.exit(main())
