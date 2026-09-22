#!/bin/bash
# Reference solution: range tombstones (memtable side table, a range-deletion
# block per table, a MANIFEST tag for tables that have one, a cached
# fragmented index for reads) and merge operands (collected by Get across
# memtables and levels, folded by DBIter in both directions, and folded by
# compactions per snapshot stripe with PartialMerge/FullMerge).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd /app/leveldb
git apply --whitespace=nowarn "${HERE}/reference.patch"
echo "reference applied"
