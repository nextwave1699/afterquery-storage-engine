#!/bin/bash
# Verifier entry point. Everything it needs is already in the image (see
# tests/Dockerfile); the only input from outside is the candidate tree at
# /app/leveldb.
#
# Writes to /logs/verifier:
#   reward.txt   the gate: 1 if every stage and guardrail passes, else 0
#   reward.json  gate_passed (same value), the objective write_amp_ratio (the
#                geometric mean over the workloads), one write_amp_ratio_<w>
#                per workload, and informational metrics (stage flags...)
#   report.json  verify.py's full report
#   pytest.txt   pytest output
#
# We write a zero reward up front and overwrite it at the end, so if anything
# blows up in between there's still a valid 0 on disk. write_amp_ratio is 1.0
# (no improvement) whenever the gate is red.
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
  "build_ok": 0,
  "functional_ok": 0,
  "differential_ok": 0,
  "crash_recovery_ok": 0,
  "compat_ok": 0,
  "guardrails_ok": 0,
  "write_amp_ratio": 1.0,
  "write_amp_ratio_mixed": 1.0,
  "write_amp_ratio_uniform": 1.0,
  "write_amp_ratio_series": 1.0,
  "write_amp_ratio_blob": 1.0,
  "write_amp": 0.0,
  "baseline_write_amp": 0.0,
  "wa_reduction": 0.0
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
def flag(name):
    return 1 if stages.get(name, {}).get("ok") is True else 0
def num(v, default=0.0):
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else default
reward = 1 if status == 0 else 0
wa = num(bench.get("candidate_wa"))
base = num(bench.get("baseline_wa"))
ratio = num(bench.get("wa_ratio"), 1.0)
out = {
    "reward": reward,
    "gate_passed": reward,
    "build_ok": flag("build"),
    "functional_ok": flag("functional"),
    "differential_ok": flag("differential"),
    "crash_recovery_ok": flag("crash"),
    "compat_ok": flag("compat"),
    "guardrails_ok": 1 if bench.get("guardrails", {}).get("ok") is True else 0,
    # the objective; only meaningful when the gate passed
    "write_amp_ratio": ratio if reward else 1.0,
    "write_amp": wa,
    "baseline_write_amp": base,
    "wa_reduction": (1.0 - wa / base) if (wa > 0 and base > 0) else 0.0,
}
# per-workload ratios, same rule: 1.0 unless the gate passed
for w in ("mixed", "uniform", "series", "blob"):
    wr = num(bench.get("workloads", {}).get(w, {}).get("wa_ratio"), 1.0)
    out["write_amp_ratio_" + w] = wr if reward else 1.0
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
