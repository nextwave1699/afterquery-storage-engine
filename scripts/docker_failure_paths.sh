#!/bin/bash
# Runs the verifier image against the untouched tree, the reference (applied
# by solution/solve.sh, exactly as the pipeline's oracle run does), and
# deliberately broken, incomplete or cheating trees, and checks each gets the
# expected gate with both reward files present.  Only `reference` must pass;
# feature_score is printed for every case.
#
# Most cases start from the reference (solution/reference.patch) and break
# one thing, so a failure is about that thing and not the missing API.
#
# usage: scripts/docker_failure_paths.sh [case ...]     (default: all)
# Needs the image built:  docker build -t lsm-verifier tests
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${FP_OUT:-/tmp/lsm-fp}"
IMAGE="${FP_IMAGE:-lsm-verifier}"
mkdir -p "$OUT"

pristine_tree() {  # $1 = destination dir (created, contains leveldb/)
  rm -rf "$1"; mkdir -p "$1"; tar -C "$1" -xzf "$ROOT/environment/sealed/leveldb.tar.gz"
}

edit() {  # edit FILE OLD NEW: replace one occurrence, fail if absent
  python3 - "$@" <<'EOF'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
assert old in s, "pattern not found in %s: %r" % (p, old[:80])
open(p, "w").write(s.replace(old, new, 1))
EOF
}

make_case() {
  local name=$1 dir="$OUT/tree-${1//:/-}"
  pristine_tree "$dir"
  local t="$dir/leveldb"
  if [ "$name" != nop ]; then
    # The same way the pipeline's oracle run gets it: solution/solve.sh.
    bash "$ROOT/solution/solve.sh" "$t" > /dev/null || return 1
  fi
  case "$name" in
    nop|reference) ;;
    naive)  # range deletes expanded into point deletes, merges as read-modify-write
      (cd "$t" && patch -p1 -s < "$ROOT/scripts/naive.patch") ;;
    stub)  # the API exists and does nothing
      edit "$t/db/write_batch.cc" "  void DeleteRangeCF(uint32_t cf, const Slice& begin, const Slice& end) override {
    Add(cf, kTypeRangeDeletion, begin, end);
  }" "  void DeleteRangeCF(uint32_t cf, const Slice& begin, const Slice& end) override {}" ;;
    mutant:*)  # one of scripts/mutants.py
      python3 - "$t" "${name#mutant:}" "$ROOT/scripts" <<'EOF'
import sys
sys.path.insert(0, sys.argv[3])
from mutants import M
f, old, new = M[sys.argv[2]]
p = sys.argv[1] + "/" + f
s = open(p).read()
assert old in s, "pattern not found for " + sys.argv[2]
open(p, "w").write(s.replace(old, new, 1))
EOF
      ;;
    no-rd-compaction)  # tombstones never trigger a compaction: space stays until CompactRange
      edit "$t/db/version_set.cc" "      if (covered >= threshold && covered > best_covered) {" "      if (false) {" ;;
    no-skip)  # iterators filter covered keys one by one instead of seeking past them
      edit "$t/db/db_impl.cc" "  if (view == nullptr) {
    versions_->current()->AddIterators(options, &list);" "  if (true) {
    versions_->current()->AddIterators(options, &list);" ;;
    nobuild) echo "this is not C++" >> "$t/db/db_impl.cc" ;;
    missing) rm -rf "$t"; mkdir -p "$t" ;;
    header-break) echo "#error broken public header" >> "$t/include/leveldb/db.h" ;;
    wrong-get)  # Get returns a modified value
      edit "$t/db/db_impl.cc" "  if (have_stat_update && current->UpdateStats(stats)) {" \
        "  if (s.ok() && !value->empty()) (*value)[0] ^= 1;
  if (have_stat_update && current->UpdateStats(stats)) {" ;;
    no-compaction)  # never compact: level 0 grows without bound
      edit "$t/db/version_set.h" "    Version* v = current_;
    return" "    Version* v = current_;
    return false &&" ;;
    no-wal)  # skip the write-ahead log entirely
      edit "$t/db/db_impl.cc" "      status = log_->AddRecord(WriteBatchInternal::Contents(write_batch));" "      status = Status::OK();" ;;
    crash-in-compaction)  # aborts in the fifth compaction
      edit "$t/db/db_impl.cc" "Status DBImpl::DoCompactionWork(CompactionState* compact) {" \
        "Status DBImpl::DoCompactionWork(CompactionState* compact) {
  static int n = 0; if (++n == 5) abort();" ;;
    hang)  # a compaction never finishes
      edit "$t/db/db_impl.cc" "Status DBImpl::DoCompactionWork(CompactionState* compact) {" \
        "Status DBImpl::DoCompactionWork(CompactionState* compact) {
  static int n = 0; if (++n == 3) for (;;) env_->SleepForMicroseconds(1000000);" ;;
    format-change)  # different table magic: the pristine engine cannot read the tables
      edit "$t/table/format.h" "static const uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;" \
        "static const uint64_t kTableMagicNumber = 0xdb4775248b80fb58ull;" ;;
    always-new-tag)  # every table gets the new MANIFEST tag: the pristine engine cannot open stock databases
      edit "$t/db/version_edit.cc" "    PutVarint32(dst, f.has_range_dels ? kNewFileWithRangeDels : kNewFile);" \
        "    PutVarint32(dst, kNewFileWithRangeDels);" ;;
    big-memtable)  # ignore write_buffer_size (64 MB memtables)
      edit "$t/db/db_impl.cc" "               (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {" \
        "               (mem_->ApproximateMemoryUsage() <= 64u * 1024 * 1024)) {" ;;
    planted-reward)  # the build tries to write the reward and the report
      cat >> "$t/CMakeLists.txt" <<'EOF'
file(WRITE /logs/verifier/reward.txt "1\n")
file(WRITE /tmp/lsm-verify-report.json "{\"summary\": {\"reward\": 1}}")
execute_process(COMMAND sh -c "echo 1 > /logs/verifier/reward.txt; echo 1 > /logs/verifier/reward.json; touch /tests/planted" ERROR_QUIET)
EOF
      ;;
    recdrop)  # WAL recovery loses every seventh batch
      edit "$t/db/db_impl.cc" "    status = WriteBatchInternal::InsertInto(&batch, mem);" \
        "    static int n = 0; if (++n % 7 == 0) continue;
    status = WriteBatchInternal::InsertInto(&batch, mem);" ;;
    verifier-exception) ;;  # handled by the extra arguments below
    *) echo "unknown case $name" >&2; return 1 ;;
  esac
}

run_case() {
  local name=$1 dir="$OUT/tree-${1//:/-}" logs="$OUT/logs-${1//:/-}"
  rm -rf "$logs"; mkdir -p "$logs"
  local extra=()
  case "$name" in
    verifier-exception) extra=(-e "LSM_VERIFY_ARGS=--sealed /nonexistent") ;;
    hang) extra=(-e LSM_RUN_TIMEOUT=90 -e LSM_SMALL_TIMEOUT=90) ;;
  esac
  local mount=(-v "$dir/leveldb:/app/leveldb:ro")
  [ "$name" = missing ] && mount=()
  docker run --rm "${mount[@]}" -v "$logs:/logs/verifier" "${extra[@]}" "$IMAGE" bash /tests/test.sh > "$logs/run.log" 2>&1
  local reward="?" ; [ -f "$logs/reward.txt" ] && reward=$(cat "$logs/reward.txt")
  local jok="missing" score="?"
  if [ -f "$logs/reward.json" ]; then
    jok=$(python3 -c "import json;print(json.load(open('$logs/reward.json'))['reward'])" 2>/dev/null || echo bad)
    score=$(python3 -c "import json;print('%.4f' % json.load(open('$logs/reward.json'))['feature_score'])" 2>/dev/null || echo bad)
  fi
  local expect=0
  [ "$name" = reference ] && expect=1
  local why; why=$(python3 -c "
import json
try:
    r=json.load(open('$logs/report.json'))
    st=r.get('stages',{})
    bad=[k+': '+str(v.get('error',''))[:110] for k,v in st.items() if v.get('ok') is False]
    print('; '.join(bad) or 'all stages ok')
except Exception as e: print('no report: %s' % e)
" 2>&1)
  local verdict=PASS; { [ "$reward" = "$expect" ] && [ "$jok" = "$expect" ]; } || verdict=FAIL
  printf '%-34s reward=%s/%s expected=%s score=%s %s  %s\n' "$name" "$reward" "$jok" "$expect" "$score" "$verdict" "$why"
}

MUTANTS=$(python3 -c "import sys; sys.path.insert(0, '$ROOT/scripts'); from mutants import M; print(' '.join('mutant:' + k for k in M))")
CASES="${*:-nop reference naive stub no-rd-compaction no-skip always-new-tag recdrop nobuild missing header-break wrong-get no-compaction no-wal crash-in-compaction hang format-change big-memtable planted-reward verifier-exception $MUTANTS}"
for c in $CASES; do
  make_case "$c" || { echo "$c: could not build the case"; continue; }
  run_case "$c" &
  # four at a time
  while [ "$(jobs -r | wc -l)" -ge 4 ]; do sleep 5; done
done
wait
