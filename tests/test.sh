#!/bin/bash
# Verifier entry point. Everything it needs is already in the image (see
# tests/Dockerfile); the only input from outside is the candidate tree at
# /app/leveldb.
#
# Writes to /logs/verifier:
#   reward.txt   the gate: 1 if every stage passes, else 0
#   reward.json  gate_passed (same value), the objective feature_score (mean
#                over the api, conformance, recovery and efficiency stages of
#                the fraction of their checks that passed), one flag per
#                stage and the counts behind the score
#   report.json  verify.py's full report
#   pytest.txt   pytest output
#
# We write a zero reward up front and overwrite it at the end, so if anything
# blows up in between there's still a valid 0 on disk.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REWARD_DIR="${LSM_REWARD_DIR:-/logs/verifier}"
WORK="${LSM_WORK:-/tmp/lsm-verify}"
TREE="${LSM_TREE:-/app/leveldb}"
export LSM_REPORT="${LSM_REPORT:-${WORK}-report.json}"
mkdir -p "${REWARD_DIR}"

write_zero() {
  rm -rf "${REWARD_DIR}/reward.txt" "${REWARD_DIR}/reward.json" 2>/dev/null || true
  printf '0\n' > "${REWARD_DIR}/reward.txt"
  cat > "${REWARD_DIR}/reward.json" <<'JSON'
{
  "reward": 0,
  "gate_passed": 0,
  "feature_score": 0.0,
  "build_ok": 0,
  "extension_api_ok": 0,
  "functional_ok": 0,
  "api_ok": 0,
  "conformance_ok": 0,
  "differential_ok": 0,
  "crash_recovery_ok": 0,
  "recovery_suite_ok": 0,
  "compat_ok": 0,
  "efficiency_ok": 0,
  "benchmark_ok": 0,
  "api_passed": 0,
  "api_total": 0,
  "scenarios_passed": 0,
  "scenarios_total": 0,
  "recovery_passed": 0,
  "recovery_total": 0,
  "efficiency_passed": 0,
  "efficiency_total": 0,
  "write_amp_ratio": 1.0
}
JSON
}
write_zero

finish() {
  status=$1
  # candidate code may have put something at these paths, so clear them
  rm -rf "${REWARD_DIR}/reward.txt" "${REWARD_DIR}/reward.json" "${REWARD_DIR}/reward.json.tmp" 2>/dev/null || true
  if [ "${status}" -eq 0 ]; then
    printf '1\n' > "${REWARD_DIR}/reward.txt"
  else
    printf '0\n' > "${REWARD_DIR}/reward.txt"
  fi
  # reward.json keys must all be flat numbers; only "reward" counts
  python3 - "${LSM_REPORT}" "${REWARD_DIR}/reward.json" "${status}" <<'PY' || write_zero
import json, os, sys
report_path, out_path, status = sys.argv[1], sys.argv[2], int(sys.argv[3])
try:
    with open(report_path) as f:
        r = json.load(f)
except Exception:
    r = {}
stages = r.get("stages", {})
bench = stages.get("benchmark", {})
summary = r.get("summary", {})
def flag(name):
    return 1 if stages.get(name, {}).get("ok") is True else 0
def num(v, default=0.0):
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else default
def count(name, key):
    return int(num(stages.get(name, {}).get(key), 0))
reward = 1 if status == 0 else 0
out = {
    "reward": reward,
    "gate_passed": reward,
    # the objective: 1.0 when every extension check passes
    "feature_score": min(1.0, max(0.0, num(summary.get("feature_score"), 0.0))),
    "build_ok": flag("build"),
    "extension_api_ok": 1 if flag("build") and not stages.get("build", {}).get("conftest_error") else 0,
    "functional_ok": flag("functional"),
    "api_ok": flag("api"),
    "conformance_ok": flag("conformance"),
    "differential_ok": flag("differential"),
    "crash_recovery_ok": flag("crash"),
    "recovery_suite_ok": flag("recovery"),
    "compat_ok": flag("compat"),
    "efficiency_ok": flag("efficiency"),
    "benchmark_ok": 1 if flag("benchmark") and bench.get("guardrails", {}).get("ok") is True else 0,
    "api_passed": count("api", "passed"),
    "api_total": count("api", "total"),
    "scenarios_passed": count("conformance", "passed"),
    "scenarios_total": count("conformance", "total"),
    "recovery_passed": count("recovery", "passed"),
    "recovery_total": count("recovery", "total"),
    "efficiency_passed": count("efficiency", "passed"),
    "efficiency_total": count("efficiency", "total"),
    # stock workloads, candidate WA / pristine WA (informational)
    "write_amp_ratio": num(bench.get("wa_ratio"), 1.0),
}
tmp = out_path + ".tmp"
with open(tmp, "w") as f:
    json.dump(out, f, indent=2, sort_keys=True)
os.replace(tmp, out_path)
PY
  cp "${LSM_REPORT}" "${REWARD_DIR}/report.json" 2>/dev/null || true
  exit 0
}
trap 'finish 1' ERR

# Ignore verify.py's exit code, pytest decides from the report.
python3 "${HERE}/verify.py" \
    --tree "${TREE}" \
    --sealed "${HERE}/sealed" \
    --work "${WORK}" \
    --out "${LSM_REPORT}" \
    --user "${LSM_USER-lsm}" \
    --measure "${LSM_MEASURE:-/opt/verifier/measure.py}" \
    ${LSM_VERIFY_ARGS:-} || true

cd "${HERE}"
python3 -m pytest -p no:cacheprovider -rA "${HERE}/test_outputs.py" > "${REWARD_DIR}/pytest.txt" 2>&1
status=$?
cat "${REWARD_DIR}/pytest.txt" || true
finish "${status}"
