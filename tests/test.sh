#!/bin/bash
# Verifier entry point.  Installs and fetches nothing: the sealed engine
# source, the fixtures, the harness and the toolchain are baked into the
# verifier image by tests/Dockerfile.  The only outside input is the
# candidate's tree, which arrives as the collected artifact at /app/leveldb.
#
# Outputs under /logs/verifier:
#   reward.txt    1 or 0: the candidate builds, every correctness stage
#                 passes (functional, differential, crash recovery, on-disk
#                 compatibility), every guardrail holds and the write
#                 amplification of the fixed workload is at or below the bar
#   reward.json   reward plus flat named metrics (write_amp, baseline_write_amp,
#                 wa_reduction, target_reached, the stage flags, guardrails)
#   report.json   the driver's full report
#   pytest.txt    the pytest transcript
#
# Both reward files are written fail-closed before anything else runs and
# rewritten at the end, so every exit path -- missing tree, build failure,
# crash, timeout, verifier bug -- leaves a parseable reward of 0.
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
  "target_reached": 0,
  "write_amp": 0.0,
  "baseline_write_amp": 0.0,
  "wa_reduction": 0.0,
  "scored_write_amp": 0.0
}
JSON
}
write_zero

# Whatever happens below, finish with a consistent pair of reward files.
finish() {
  status=$1
  # A candidate process could have replaced these paths by directories; clear them first.
  rm -rf "${REWARD_DIR}/reward.txt" "${REWARD_DIR}/reward.json" "${REWARD_DIR}/reward.json.tmp" 2>/dev/null || true
  if [ "${status}" -eq 0 ]; then
    printf '1\n' > "${REWARD_DIR}/reward.txt"
  else
    printf '0\n' > "${REWARD_DIR}/reward.txt"
  fi
  # reward.json: reward equals the gate; the other keys are informational
  # and must be flat numbers.
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
target = 1 if bench.get("target_reached") is True and reward else 0
out = {
    "reward": reward,
    "gate_passed": reward,
    "build_ok": flag("build"),
    "functional_ok": flag("functional"),
    "differential_ok": flag("differential"),
    "crash_recovery_ok": flag("crash"),
    "compat_ok": flag("compat"),
    "guardrails_ok": 1 if bench.get("guardrails", {}).get("ok") is True else 0,
    "target_reached": target,
    "write_amp": wa,
    "baseline_write_amp": base,
    "wa_reduction": (1.0 - wa / base) if (wa > 0 and base > 0) else 0.0,
    # the objective as scored: the measured write amplification once the
    # gate is passed, otherwise 0 (no credit)
    "scored_write_amp": wa if reward else 0.0,
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

# The driver's exit status is not the verdict; the report is, through pytest.
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
