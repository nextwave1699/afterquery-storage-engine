#!/bin/bash
# Runs the verifier image against deliberately broken, cheating or merely
# insufficient trees and checks that every one of them yields reward 0 with
# both reward files present, plus nop (0) and oracle (1).
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

make_case() {
  local name=$1 dir="$OUT/tree-$1"
  pristine_tree "$dir"
  local t="$dir/leveldb"
  case "$name" in
    nop) ;;
    oracle) (cd "$t" && patch -p1 -s < "$ROOT/solution/compaction.patch") ;;
    nobuild) echo "this is not C++" >> "$t/db/db_impl.cc" ;;
    missing) rm -rf "$t"; mkdir -p "$t" ;;
    header-break) echo "#error broken public header" >> "$t/include/leveldb/db.h" ;;
    wrong-get)  # Get returns a modified value
      python3 - "$t/db/db_impl.cc" <<'EOF'
import sys; p=sys.argv[1]; s=open(p).read()
old="  if (have_stat_update && current->UpdateStats(stats)) {"
assert old in s
s=s.replace(old,"  if (s.ok() && !value->empty()) (*value)[0] ^= 1;\n"+old)
open(p,"w").write(s)
EOF
      ;;
    scan-bug)  # iterators do not hide deletions
      python3 - "$t/db/db_iter.cc" <<'EOF'
import sys; p=sys.argv[1]; s=open(p).read()
old="""        case kTypeDeletion:
          // Arrange to skip all upcoming entries for this key since
          // they are hidden by this deletion.
          SaveKey(ikey.user_key, skip);
          skipping = true;
          break;"""
assert old in s
s=s.replace(old,"""        case kTypeDeletion:
          valid_ = true;
          saved_key_.clear();
          return;""")
open(p,"w").write(s)
EOF
      ;;
    no-compaction)  # never compact: level 0 grows without bound
      python3 - "$t/db/version_set.h" <<'EOF'
import sys; p=sys.argv[1]; s=open(p).read()
old="    return (v->compaction_score_ >= 1) || (v->file_to_compact_ != nullptr);"
assert old in s
s=s.replace(old,"    return false;")
open(p,"w").write(s)
EOF
      ;;
    no-wal)  # skip the write-ahead log entirely
      python3 - "$t/db/db_impl.cc" <<'EOF'
import sys; p=sys.argv[1]; s=open(p).read()
old="      status = log_->AddRecord(WriteBatchInternal::Contents(write_batch));"
assert old in s
s=s.replace(old,"      status = Status::OK();")
open(p,"w").write(s)
EOF
      ;;
    crash-in-compaction)  # aborts in the fifth compaction
      python3 - "$t/db/db_impl.cc" <<'EOF'
import sys; p=sys.argv[1]; s=open(p).read()
old="Status DBImpl::DoCompactionWork(CompactionState* compact) {"
assert old in s
s=s.replace(old, old+"\n  static int n = 0; if (++n == 5) abort();")
open(p,"w").write(s)
EOF
      ;;
    hang)  # a compaction never finishes
      python3 - "$t/db/db_impl.cc" <<'EOF'
import sys; p=sys.argv[1]; s=open(p).read()
old="Status DBImpl::DoCompactionWork(CompactionState* compact) {"
assert old in s
s=s.replace(old, old+"\n  static int n = 0; if (++n == 3) for (;;) env_->SleepForMicroseconds(1000000);")
open(p,"w").write(s)
EOF
      ;;
    format-change)  # different table magic: pristine engine cannot read the tables
      python3 - "$t/table/format.h" <<'EOF'
import sys; p=sys.argv[1]; s=open(p).read()
old="static const uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;"
assert old in s
s=s.replace(old,"static const uint64_t kTableMagicNumber = 0xdb4775248b80fb58ull;")
open(p,"w").write(s)
EOF
      ;;
    big-memtable)  # ignore write_buffer_size (64 MB memtables)
      python3 - "$t/db/db_impl.cc" <<'EOF'
import sys; p=sys.argv[1]; s=open(p).read()
old="               (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {"
assert old in s
s=s.replace(old,"               (mem_->ApproximateMemoryUsage() <= 64u * 1024 * 1024)) {")
open(p,"w").write(s)
EOF
      ;;
    seek-only)  # the trivial change alone: no seek compactions (~6.1, above the bar)
      python3 - "$t/db/version_set.cc" <<'EOF'
import sys; p=sys.argv[1]; s=open(p).read()
old="bool Version::UpdateStats(const GetStats& stats) {\n  FileMetaData* f = stats.seek_file;\n  if (f != nullptr) {"
assert old in s
s=s.replace(old,"bool Version::UpdateStats(const GetStats& stats) {\n  FileMetaData* f = stats.seek_file;\n  if (f != nullptr && false) {")
open(p,"w").write(s)
EOF
      ;;
    tiered-constants)  # seek-only + L0 trigger 12 + L1 40 MB: low WA, level-0 depth 12
      python3 - "$t/db/version_set.cc" "$t/db/dbformat.h" <<'EOF'
import sys; p=sys.argv[1]; s=open(p).read()
s=s.replace("  if (f != nullptr) {\n    f->allowed_seeks--;","  if (f != nullptr && false) {\n    f->allowed_seeks--;")
s=s.replace("double result = 10. * 1048576.0;","double result = 40. * 1048576.0;")
open(p,"w").write(s)
p=sys.argv[2]; s=open(p).read()
s=s.replace("kL0_CompactionTrigger = 4","kL0_CompactionTrigger = 12").replace("kL0_SlowdownWritesTrigger = 8","kL0_SlowdownWritesTrigger = 16").replace("kL0_StopWritesTrigger = 12","kL0_StopWritesTrigger = 20")
open(p,"w").write(s)
EOF
      ;;
    planted-reward)  # the build tries to write the reward and the report
      cat >> "$t/CMakeLists.txt" <<'EOF'
file(WRITE /logs/verifier/reward.txt "1\n")
file(WRITE /tmp/lsm-verify-report.json "{\"summary\": {\"reward\": 1}}")
execute_process(COMMAND sh -c "echo 1 > /logs/verifier/reward.txt; echo 1 > /logs/verifier/reward.json; touch /tests/planted" ERROR_QUIET)
EOF
      ;;
    verifier-exception) ;;  # handled by the extra arguments below
    *) echo "unknown case $name" >&2; return 1 ;;
  esac
}

run_case() {
  local name=$1 dir="$OUT/tree-$1" logs="$OUT/logs-$1"
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
  local jok="missing"; [ -f "$logs/reward.json" ] && jok=$(python3 -c "import json;print(json.load(open('$logs/reward.json'))['reward'])" 2>/dev/null || echo bad)
  local expect=0; [ "$name" = oracle ] && expect=1
  local why; why=$(python3 -c "
import json
try:
    r=json.load(open('$logs/report.json'))
    st=r.get('stages',{})
    bad=[k+': '+str(v.get('error',''))[:90] for k,v in st.items() if v.get('ok') is False]
    b=st.get('benchmark',{})
    print('; '.join(bad) or ('wa=%.3f guard=%s' % (b.get('candidate_wa') or 0, b.get('guardrails',{}).get('failures'))))
except Exception as e: print('no report: %s' % e)
" 2>&1)
  local verdict=PASS; { [ "$reward" = "$expect" ] && [ "$jok" = "$expect" ]; } || verdict=FAIL
  printf '%-22s reward.txt=%s reward.json=%s expected=%s %s  %s\n' "$name" "$reward" "$jok" "$expect" "$verdict" "$why"
}

CASES="${*:-nop oracle nobuild missing header-break wrong-get scan-bug no-compaction no-wal crash-in-compaction hang format-change big-memtable seek-only tiered-constants planted-reward verifier-exception}"
for c in $CASES; do
  make_case "$c" || continue
  run_case "$c" &
  # four at a time
  while [ "$(jobs -r | wc -l)" -ge 4 ]; do sleep 5; done
done
wait
