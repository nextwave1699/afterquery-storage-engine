#!/bin/bash
# The reference implementation: range tombstones (a memtable fragment map, a
# range-deletion block per table, a MANIFEST tag for the tables that have
# one, layer-tagged indexes so reads skip what a newer layer hides), merge
# operands (collected across memtables and levels, folded by DBIter in both
# directions and by compactions per snapshot stripe), and column families
# (one memtable, level set, table cache and directory each, sharing the log,
# the sequence numbers and the snapshots).
#
# The files are shipped as a tarball, not a patch: an upload that rewrites
# line endings would make a patch fail to apply, and a half-applied tree
# would look like a wrong answer instead of a packaging problem.  The patch
# (reference.patch) is kept beside it for reading.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE="${1:-/app/leveldb}"

test -d "${TREE}" || { echo "solve.sh: ${TREE} is not a directory" >&2; exit 1; }

if [ -f "${HERE}/reference-files.tar.gz" ]; then
  tar -C "${TREE}" -xzf "${HERE}/reference-files.tar.gz"
else
  # Fall back to the patch, with any carriage returns an upload may have
  # introduced taken back out.
  tr -d '\r' < "${HERE}/reference.patch" > /tmp/reference.patch
  (cd "${TREE}" && git apply --whitespace=nowarn /tmp/reference.patch) ||
    (cd "${TREE}" && patch -p1 -s < /tmp/reference.patch)
fi

# The tree must now carry the new API; stop loudly if anything is missing.
missing=0
for f in include/leveldb/merge_operator.h db/column_family.cc db/column_family.h \
         db/range_del.cc db/range_del_view.h db/lookup_state.h; do
  if [ ! -f "${TREE}/${f}" ]; then
    echo "solve.sh: ${f} is missing after unpacking" >&2
    missing=1
  fi
done
for pattern in 'DeleteRange' 'ColumnFamilyHandle' 'merge_operator'; do
  if ! grep -q "${pattern}" "${TREE}/include/leveldb/db.h" "${TREE}/include/leveldb/options.h"; then
    echo "solve.sh: ${pattern} is not in the public headers after unpacking" >&2
    missing=1
  fi
done
if [ "${missing}" -ne 0 ]; then
  exit 1
fi

echo "reference unpacked into ${TREE} ($(tar tzf "${HERE}/reference-files.tar.gz" | wc -l) files)"
