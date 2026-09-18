#!/usr/bin/env python3
"""Verifier for the storage-engine task.  Runs as root inside the verifier
container; everything that executes candidate code runs as the unprivileged
user `lsm`.

Pipeline (each stage is skipped when a stage it depends on failed):

  build      pristine engine + harness (trusted), candidate engine (as lsm)
             + harness compiled from the verifier's own sources against it
  functional selftest through the public API
  differential  small workload: candidate writes, pristine reads; pristine
             writes, candidate reads; every read checked against the model
  crash      SIGKILL at engine events (WAL append, table close, manifest
             append, obsolete-file removal, mid-table write, CURRENT rename
             during reopen, recovery); both engines recover and agree; the
             directory is left consistent; torn WAL tail
  compat     sealed databases written by the pristine engine (clean close,
             live WAL, torn WAL, reuse_logs, plain tables) plus one generated
             with a fresh seed; candidate reads them, extends them, pristine
             reads the result
  benchmark  the fixed workload at full scale with a seed drawn now: pristine
             and candidate, kernel-measured bytes written / logical bytes;
             cross reads; shape guardrails from the MANIFEST history
  threshold  write amplification <= WA_MAX and every guardrail

Everything is written to the report (--out) incrementally; the reward files
are produced by test.sh from the report and pytest.
"""
import argparse
import json
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

# ------------------------------------------------------------------ contract

WA_MAX = 5.40                  # the bar: total bytes written / logical bytes
BASELINE_WA_BAND = (11.0, 22.0)  # sanity band for the pristine engine
BENCH_SCALE = 3.0
SMALL_SCALE = 0.25
CRASH_SCALE = 0.3
FIXTURE_SCALE = 0.15
MAX_L0_DEPTH = 4               # level-0 files overlapping one key, at any version
SPACE_RATIO_MAX = 1.25         # peak table bytes vs. pristine
READ_RATIO_MAX = 1.5           # phase-F bytes read vs. pristine
FILES_RATIO_MAX = 3.0          # peak file count vs. pristine
MAX_FILE_SIZE = 8 * 1024 * 1024
RSS_EXTRA_MAX_KB = 96 * 1024   # peak RSS vs. pristine
# Unreferenced tables may linger until the next compaction or open (LevelDB
# only removes files no live version uses), but never accumulate.
ORPHAN_RATIO_MAX = 0.5
ORPHAN_SLACK = 16 * 1024 * 1024
DISK_RATIO_MAX = 1.5           # table bytes on disk after the run vs. pristine (gross garbage)
MEMTABLE_SWITCH_RATIO_MIN = 0.8
TIME_RATIO_MAX = 4.0           # wall time vs. pristine (plus 60 s)
BUILD_TIMEOUT = int(os.environ.get("LSM_BUILD_TIMEOUT", "900"))
RUN_TIMEOUT = int(os.environ.get("LSM_RUN_TIMEOUT", "900"))
SMALL_TIMEOUT = int(os.environ.get("LSM_SMALL_TIMEOUT", "600"))


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
            "wa_max": WA_MAX, "bench_scale": BENCH_SCALE, "max_l0_depth": MAX_L0_DEPTH,
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
        rng = random.SystemRandom()
        self.seed = args.seed if args.seed else rng.randint(1000, 999999)
        self.small_seed = self.seed + 7
        self.report["seed"] = self.seed
        self.t0 = time.time()

    # ---------------------------------------------------------------- utils

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
        """Give `path` (recursively) to the unprivileged user."""
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
        """Run a command as the unprivileged user; returns (status, stdout, stderr)."""
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
        """Run a harness command through measure.py as the user; returns the
        measurement dict plus stdout/stderr."""
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
            except Exception as e:  # noqa
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

    def check(self, which, db, seed, scale, batches, cont=0, paranoid=True, name=None, timeout=SMALL_TIMEOUT):
        extra = ["--batches", str(batches)]
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
        """Directory consistency: metadata parses, every referenced table is
        present with its recorded size, unreferenced leftovers are bounded,
        no obsolete WAL accumulates.  Returns the summary."""
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

    def describe(self, seed, scale):
        status, out, err = self.run_as_user([self.ref_bin, "describe", "--seed", str(seed), "--scale", str(scale)],
                                            timeout=300)
        if status != 0:
            raise Fail("describe failed: %s" % err[-300:])
        return json.loads(out.strip().splitlines()[-1])

    # ---------------------------------------------------------------- stages

    def build(self):
        st = self.report["stages"]["build"] = {"ok": None}
        os.makedirs(os.path.join(self.work, "logs"), exist_ok=True)
        os.makedirs(os.path.join(self.work, "meas"), exist_ok=True)
        self.own(os.path.join(self.work, "meas"))
        # Pristine tree from the sealed archive.
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
        # Candidate tree: copy, give to the user, build as the user.
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
        # The harness is always compiled by the verifier from its own sources.
        binary = os.path.join(self.work, "lsmbench-" + tag)
        hs = os.path.join(HERE, "harness")
        cc = ["g++", "-std=c++17", "-O2", "-fno-rtti", "-I" + os.path.join(tree, "include"), "-I" + hs,
              os.path.join(hs, "lsmbench.cc"), "-o", binary, lib, "-lpthread"]
        status, out, err = self.run_as_user(cc, BUILD_TIMEOUT, log_name="harness-%s.log" % tag, as_root=True)
        if status != 0 or not os.path.isfile(binary):
            raise Fail("harness does not compile against the %s tree (status %s): %s" % (tag, status, err[-1500:]))
        os.chmod(binary, 0o755)
        return binary

    def functional(self):
        st = self.report["stages"]["functional"] = {"ok": None}
        db = self.udir("db-selftest")
        m = self.harness("cand", "selftest", db, 1, 1, name="cand-selftest")
        self.expect_ok(m, "selftest")
        st["ok"] = True

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
        # name, extra run args.  --arm-at-batch N starts counting events after
        # batch N was acknowledged; EVENT:K kills before the K-th event (":after"
        # right after it).  The points fall in different flush/compaction cycles.
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
                # Simulate a torn write: cut the tail of the live WAL.  Only
                # the last record may be lost.
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
            # 1. The pristine engine recovers the candidate's crashed directory
            #    (its WAL, MANIFEST, CURRENT, tables) on a copy.
            copy = self.copy_db(db, "db-crash-%s-refcopy" % name)
            a_ref, _, _ = self.check("ref", copy, seed, scale, expect, name="crash-%s-ref" % name)
            shutil.rmtree(copy, ignore_errors=True)
            # 2. The candidate recovers, is checked, continues, closes.
            a_cand, final, _ = self.check("cand", db, seed, scale, expect, cont=3000, name="crash-%s-cand" % name)
            sc["recovered"] = a_cand
            if a_cand != a_ref:
                raise Fail("crash %s: pristine recovered %d batches, candidate %d" % (name, a_ref, a_cand))
            if final is None or final < a_cand + 1:
                raise Fail("crash %s: candidate did not continue after recovery" % name)
            # 3. Pristine reads the continued database, candidate reopens it after that.
            a2, _, _ = self.check("ref", db, seed, scale, final, name="crash-%s-ref2" % name)
            if a2 != final:
                raise Fail("crash %s: pristine reads %d batches after continuation, expected %d" % (name, a2, final))
            a3, _, _ = self.check("cand", db, seed, scale, final, name="crash-%s-cand2" % name)
            if a3 != final:
                raise Fail("crash %s: candidate reads %d batches after the pristine reopen, expected %d" % (name, a3, final))
            # 4. Directory hygiene: metadata parses, every referenced table
            #    exists, nothing unreferenced or stale is left behind.
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
        # A fixture produced right now by the pristine engine with a fresh seed.
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
        st = self.report["stages"]["benchmark"] = {"ok": None}
        seed, scale = self.seed, BENCH_SCALE
        desc = self.describe(seed, scale)
        logical = desc["logical_bytes"]
        st["logical_bytes"] = logical
        st["batches"] = desc["batches"]
        results = {}
        for which in ("ref", "cand"):
            db = self.udir("db-bench-" + which)
            r = results[which] = {}
            t = time.time()
            m = self.harness(which, "run", db, seed, scale, ["--stats", os.path.join(self.work, "meas", "stats-%s-run.json" % which)],
                             timeout=RUN_TIMEOUT, name="bench-%s-run" % which)
            self.expect_ok(m, "%s benchmark run" % which)
            r["run"] = strip(m)
            info_run = self.hygiene(db, "%s benchmark run" % which)   # history of the whole run
            r["manifest_run"] = {k: info_run[k] for k in ("levels", "level_bytes", "max_l0", "max_l0_depth",
                                                          "max_files", "max_file_size", "max_total_bytes",
                                                          "mean_total_bytes", "edits", "files_added", "bytes_added",
                                                          "memtable_switches", "table_bytes_present",
                                                          "orphan_bytes", "referenced_bytes")}
            m = self.harness(which, "read", db, seed, scale, ["--stats", os.path.join(self.work, "meas", "stats-%s-read.json" % which)],
                             timeout=RUN_TIMEOUT, name="bench-%s-read" % which)
            self.expect_ok(m, "%s benchmark read phase" % which)
            r["read"] = strip(m)
            r["wall_s"] = time.time() - t
            r["written"] = r["run"]["wchar"] + r["read"]["wchar"]
            r["wa"] = r["written"] / float(logical)
            r["read_bytes_f"] = r["read"]["rchar"]
            r["maxrss_kb"] = max(r["run"]["maxrss_kb"], r["read"]["maxrss_kb"])
            self.log("%s: write amplification %.3f (%.0f MB written / %.0f MB logical), phase F read %.0f MB, rss %d MB, %.0fs" % (
                which, r["wa"], r["written"] / 1e6, logical / 1e6, r["read_bytes_f"] / 1e6, r["maxrss_kb"] // 1024, r["wall_s"]))
            self.save()
        # cross reads at full scale: pristine reads the candidate's database
        # and the candidate reads the pristine's.
        a, _, _ = self.check("ref", os.path.join(self.work, "db-bench-cand"), seed, scale, desc["batches"],
                             name="bench-ref-reads-cand", timeout=RUN_TIMEOUT)
        if a != desc["batches"]:
            raise Fail("pristine reader saw %d batches in the candidate's benchmark database" % a)
        a, _, _ = self.check("cand", os.path.join(self.work, "db-bench-ref"), seed, scale, desc["batches"],
                             name="bench-cand-reads-ref", timeout=RUN_TIMEOUT)
        if a != desc["batches"]:
            raise Fail("candidate reader saw %d batches in the pristine benchmark database" % a)
        st["results"] = results
        base, cand = results["ref"], results["cand"]
        st["baseline_wa"] = base["wa"]
        st["candidate_wa"] = cand["wa"]
        if not (BASELINE_WA_BAND[0] <= base["wa"] <= BASELINE_WA_BAND[1]):
            raise Fail("verifier self-check: pristine write amplification %.2f outside %s" % (base["wa"], BASELINE_WA_BAND))
        # ---- guardrails
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
        if g["disk_ratio"] > DISK_RATIO_MAX:
            fails.append("table bytes on disk after the run %.2fx baseline > %.2f" % (g["disk_ratio"], DISK_RATIO_MAX))
        if g["max_l0_depth"] > MAX_L0_DEPTH:
            fails.append("level-0 depth %d > %d" % (g["max_l0_depth"], MAX_L0_DEPTH))
        if g["space_ratio"] > SPACE_RATIO_MAX:
            fails.append("peak space %.2fx baseline > %.2f" % (g["space_ratio"], SPACE_RATIO_MAX))
        if g["read_ratio"] > READ_RATIO_MAX:
            fails.append("phase-F read bytes %.2fx baseline > %.2f" % (g["read_ratio"], READ_RATIO_MAX))
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
        st["target_reached"] = cand["wa"] <= WA_MAX
        st["ok"] = True
        if fails:
            self.log("guardrail failures: " + "; ".join(fails))
        self.log("candidate WA %.3f (bar %.2f, baseline %.3f): %s" % (
            cand["wa"], WA_MAX, base["wa"], "target reached" if st["target_reached"] else "target NOT reached"))

    # ---------------------------------------------------------------- driver

    def run(self):
        self.save()
        with self.stage("build"):
            self.build()
        if self.stage_ok("build"):
            with self.stage("functional"):
                self.functional()
        if self.stage_ok("functional"):
            with self.stage("differential"):
                self.differential()
        if self.stage_ok("differential"):
            with self.stage("crash"):
                self.crash()
            with self.stage("compat"):
                self.compat()
        if self.stage_ok("compat") and self.stage_ok("crash"):
            with self.stage("benchmark"):
                self.benchmark()
        s = self.report["stages"]
        correctness = all(self.stage_ok(n) for n in ("build", "functional", "differential", "crash", "compat"))
        bench = s.get("benchmark", {})
        guard_ok = bool(bench.get("ok")) and bool(bench.get("guardrails", {}).get("ok"))
        target = bool(bench.get("target_reached"))
        self.report["summary"] = {
            "correctness_passed": correctness,
            "guardrails_passed": guard_ok,
            "target_reached": target,
            "reward": 1 if (correctness and guard_ok and target) else 0,
            "candidate_wa": bench.get("candidate_wa"),
            "baseline_wa": bench.get("baseline_wa"),
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
        return True  # never propagate


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
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    v = Verifier(args)
    try:
        return v.run()
    except Exception:  # noqa: last resort: still leave a report
        v.report["fatal"] = traceback.format_exc()
        v.report.setdefault("summary", {"reward": 0, "correctness_passed": False,
                                        "guardrails_passed": False, "target_reached": False})
        v.save()
        return 1


if __name__ == "__main__":
    sys.exit(main())
