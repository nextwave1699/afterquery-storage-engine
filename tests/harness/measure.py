#!/usr/bin/env python3
"""Run one harness process and report the kernel's view of it.

Runs as the same (unprivileged) user as the harness.  The harness is started
with --stop-at-end: on success it stops itself (SIGSTOP) instead of exiting,
this helper reads /proc/<pid>/io (bytes passed to write(2)/read(2) by every
thread of the process, maintained by the kernel), /proc/<pid>/status (peak
RSS) and then kills it with SIGKILL, so nothing that runs after main()
(atexit handlers, static destructors) can add or hide I/O.  The rusage of
the reaped child gives the peak RSS again and the CPU time.

usage: measure.py OUT.json TIMEOUT_SEC -- cmd args...
Output JSON: stopped (bool), exit_status, signal, wchar, rchar, write_bytes,
read_bytes, maxrss_kb, utime, stime, wall_s, timed_out.
"""
import json
import os
import signal
import subprocess
import sys
import time


def main():
    out_path = sys.argv[1]
    timeout = float(sys.argv[2])
    assert sys.argv[3] == "--"
    cmd = sys.argv[4:] + ["--stop-at-end"]
    result = {"stopped": False, "timed_out": False, "exit_status": None, "signal": None,
              "wchar": 0, "rchar": 0, "write_bytes": 0, "read_bytes": 0,
              "maxrss_kb": 0, "utime": 0.0, "stime": 0.0, "wall_s": 0.0}
    t0 = time.time()
    # New session so a timeout can kill the whole process group.
    p = subprocess.Popen(cmd, start_new_session=True)
    deadline = t0 + timeout
    info = None
    while True:
        try:
            info = os.waitid(os.P_PID, p.pid, os.WSTOPPED | os.WEXITED | os.WNOHANG | os.WNOWAIT)
        except ChildProcessError:
            info = None
            break
        if info is not None and info.si_pid == p.pid:
            break
        if time.time() > deadline:
            result["timed_out"] = True
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            break
        time.sleep(0.02)
    result["wall_s"] = round(time.time() - t0, 3)
    if info is not None and info.si_code == os.CLD_STOPPED and not result["timed_out"]:
        result["stopped"] = True
        try:
            with open("/proc/%d/io" % p.pid) as f:
                for line in f:
                    k, v = line.split(":")
                    k = k.strip()
                    if k in result:
                        result[k] = int(v)
        except OSError as e:
            result["io_error"] = str(e)
        try:
            with open("/proc/%d/status" % p.pid) as f:
                for line in f:
                    if line.startswith("VmHWM:"):
                        result["vmhwm_kb"] = int(line.split()[1])
        except OSError:
            pass
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    try:
        _, status, ru = os.wait4(p.pid, 0)
    except ChildProcessError:
        status, ru = None, None
    if status is not None:
        if os.WIFEXITED(status):
            result["exit_status"] = os.WEXITSTATUS(status)
        elif os.WIFSIGNALED(status):
            result["signal"] = os.WTERMSIG(status)
    if ru is not None:
        result["maxrss_kb"] = max(result["maxrss_kb"], ru.ru_maxrss)
        result["utime"] = ru.ru_utime
        result["stime"] = ru.ru_stime
    with open(out_path, "w") as f:
        json.dump(result, f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
