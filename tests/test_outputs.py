"""Checks on the report verify.py writes to $LSM_REPORT.

These are the gate: test.sh sets gate_passed (and reward.txt) to 1 only if
all of them pass.  The objective, feature_score, is reported separately."""
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
    st = stage(report, "build")
    assert not st.get("conftest_error"), st.get("conftest_error", "")[:2000]


def test_api(report):
    """The fixed checks of the range-delete and merge API pass."""
    st = stage(report, "api")
    assert st.get("passed") == st.get("total") and st.get("total")


def test_functional(report):
    """lsmbench selftest passes on the candidate."""
    stage(report, "functional")


def test_conformance(report):
    """Every conformance scenario passes: reads, iterator walks, snapshots,
    reopens and compactions all match the model."""
    st = stage(report, "conformance")
    assert st.get("passed") == st.get("total") and st.get("total"), \
        "%s of %s scenarios passed" % (st.get("passed"), st.get("total"))


def test_recovery(report):
    """Every randomized crash is recovered to an acknowledged state (the same
    one by both engines where both can read the database)."""
    st = stage(report, "recovery")
    assert st.get("passed") == st.get("total") and st.get("total")


def test_efficiency(report):
    """Range deletes and merges stay within their I/O limits."""
    st = stage(report, "efficiency")
    assert st.get("passed") == st.get("total") and st.get("total")


def test_differential(report):
    """Each engine can read what the other wrote on a small workload."""
    stage(report, "differential")


def test_crash_recovery(report):
    """Every crash scenario recovers all acked batches with both engines."""
    st = stage(report, "crash")
    for name, sc in st.get("scenarios", {}).items():
        assert sc.get("ok") is True, "crash scenario %s did not pass" % name


def test_compat(report):
    """Candidate reads and extends pristine-written dbs, pristine reads back."""
    st = stage(report, "compat")
    assert len(st.get("fixtures", {})) >= 6


def test_benchmark_ran(report):
    """Every stock workload ran on both engines and was measured."""
    st = stage(report, "benchmark")
    assert st.get("wa_ratio") is not None
    want = report.get("contract", {}).get("workloads") or []
    got = st.get("workloads", {})
    assert want and all(got.get(w, {}).get("wa_ratio") is not None for w in want), \
        "missing workloads: %s" % [w for w in want if w not in got]


def test_guardrails(report):
    """No regression on the stock workloads: write amplification, L0 depth,
    space, reads, files, RSS, memtables and time within limits."""
    st = stage(report, "benchmark")
    g = st.get("guardrails", {})
    assert g.get("ok") is True, "guardrail failures: %s" % g.get("failures")

