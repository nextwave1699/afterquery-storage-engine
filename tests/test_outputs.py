"""The verdict: every check reads the report written by verify.py (path in
$LSM_REPORT).  All tests must pass for reward 1; test.sh maps the pytest
status to the reward files."""
import json
import os

import pytest

REPORT = os.environ.get("LSM_REPORT", "/tmp/lsm-verify-report.json")


@pytest.fixture(scope="module")
def report():
    if not os.path.exists(REPORT):
        pytest.fail("verifier produced no report at %s" % REPORT)
    with open(REPORT) as f:
        return json.load(f)


def stage(report, name):
    st = report.get("stages", {}).get(name)
    if st is None:
        pytest.fail("stage %s did not run" % name)
    if st.get("ok") is not True:
        pytest.fail("stage %s failed: %s" % (name, st.get("error", "unknown")))
    return st


def test_build(report):
    """The candidate engine builds and the harness compiles against it."""
    stage(report, "build")


def test_functional(report):
    """Basic operations through the public API behave."""
    stage(report, "functional")


def test_differential(report):
    """Candidate and pristine engines agree on a small workload in both
    directions (each reads what the other wrote), every read checked."""
    stage(report, "differential")


def test_crash_recovery(report):
    """After SIGKILL at each engine event, acknowledged writes are durable,
    both engines recover the same state, the directory is consistent."""
    st = stage(report, "crash")
    for name, sc in st.get("scenarios", {}).items():
        assert sc.get("ok") is True, "crash scenario %s did not pass" % name


def test_compat(report):
    """Databases written by the pristine engine (sealed and fresh) are read
    and extended by the candidate, and the result read by the pristine."""
    st = stage(report, "compat")
    assert len(st.get("fixtures", {})) >= 6


def test_benchmark_ran(report):
    """The fixed workload completed for both engines with cross reads."""
    st = stage(report, "benchmark")
    assert st.get("candidate_wa") is not None


def test_guardrails(report):
    """Level-0 depth, peak space, read bytes, file count/size, memory,
    memtable switches and wall time stay inside the envelope."""
    st = stage(report, "benchmark")
    g = st.get("guardrails", {})
    assert g.get("ok") is True, "guardrail failures: %s" % g.get("failures")


def test_write_amplification(report):
    """Total bytes written / logical bytes is at or below the bar."""
    st = stage(report, "benchmark")
    wa = st.get("candidate_wa")
    bar = report.get("contract", {}).get("wa_max")
    assert wa is not None and bar is not None
    assert wa <= bar, "write amplification %.3f exceeds the bar %.2f (baseline %.3f)" % (
        wa, bar, st.get("baseline_wa") or 0.0)
