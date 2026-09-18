"""Trusted reader of a LevelDB database directory's metadata.

Parses CURRENT and the MANIFEST it points at (log-format records of
VersionEdits) with no help from the engine, replays the edits and reports
the level layout after every edit.  Used by the verifier to check the shape
of what the candidate produced (files per level over time, file sizes,
referenced vs. present files) and, indirectly, that the manifest format is
the one the pristine engine writes.

The formats (db/log_format.h, db/version_edit.cc of LevelDB 1.23):

  log block  = 32768 bytes; record header = crc32c(4) length(2) type(1)
  record types: 1 FULL, 2 FIRST, 3 MIDDLE, 4 LAST (0 = zero/padding)
  VersionEdit tags: 1 comparator(str) 2 log_number(varint) 3 next_file(varint)
    4 last_sequence(varint) 5 compact_pointer(level varint, key str)
    6 deleted_file(level varint, number varint)
    7 new_file(level varint, number varint, size varint, smallest str, largest str)
    9 prev_log_number(varint)
"""
import os
import re
import struct
import zlib

BLOCK_SIZE = 32768
HEADER_SIZE = 7

# crc32c with LevelDB's mask.
_CRC_TABLE = None


def _crc32c_table():
    global _CRC_TABLE
    if _CRC_TABLE is None:
        t = []
        for i in range(256):
            c = i
            for _ in range(8):
                c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
            t.append(c)
        _CRC_TABLE = t
    return _CRC_TABLE


def crc32c(data, crc=0):
    t = _crc32c_table()
    crc ^= 0xFFFFFFFF
    for b in data:
        crc = t[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def unmask(masked):
    rot = (masked - 0xA282EAD8) & 0xFFFFFFFF
    return ((rot >> 17) | (rot << 15)) & 0xFFFFFFFF


class ManifestError(Exception):
    pass


def read_log_records(data, verify_crc=True):
    """Yield the records of a LevelDB log-format file."""
    records = []
    pos = 0
    n = len(data)
    fragment = b""
    in_fragment = False
    while pos + HEADER_SIZE <= n:
        block_left = BLOCK_SIZE - (pos % BLOCK_SIZE)
        if block_left < HEADER_SIZE:
            pos += block_left
            continue
        crc_masked, length, rtype = struct.unpack("<IHB", data[pos:pos + HEADER_SIZE])
        if rtype == 0 and length == 0:
            # zero padding at the end of a block
            pos += block_left
            continue
        payload = data[pos + HEADER_SIZE:pos + HEADER_SIZE + length]
        if len(payload) < length:
            raise ManifestError("truncated record at offset %d" % pos)
        if verify_crc:
            expect = crc32c(bytes([rtype]) + payload)
            if unmask(crc_masked) != expect:
                raise ManifestError("bad crc at offset %d" % pos)
        pos += HEADER_SIZE + length
        if rtype == 1:  # full
            records.append(payload)
            in_fragment = False
        elif rtype == 2:  # first
            fragment = payload
            in_fragment = True
        elif rtype == 3:  # middle
            if not in_fragment:
                raise ManifestError("middle fragment without first")
            fragment += payload
        elif rtype == 4:  # last
            if not in_fragment:
                raise ManifestError("last fragment without first")
            records.append(fragment + payload)
            in_fragment = False
        else:
            raise ManifestError("unknown record type %d" % rtype)
    return records


def read_varint(data, pos):
    result = 0
    shift = 0
    while True:
        if pos >= len(data):
            raise ManifestError("truncated varint")
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if b < 0x80:
            return result, pos
        shift += 7
        if shift > 63:
            raise ManifestError("varint too long")


def read_slice(data, pos):
    length, pos = read_varint(data, pos)
    if pos + length > len(data):
        raise ManifestError("truncated slice")
    return data[pos:pos + length], pos + length


def decode_edit(rec):
    edit = {"new_files": [], "deleted_files": [], "compact_pointers": []}
    pos = 0
    while pos < len(rec):
        tag, pos = read_varint(rec, pos)
        if tag == 1:
            edit["comparator"], pos = read_slice(rec, pos)
        elif tag == 2:
            edit["log_number"], pos = read_varint(rec, pos)
        elif tag == 3:
            edit["next_file_number"], pos = read_varint(rec, pos)
        elif tag == 4:
            edit["last_sequence"], pos = read_varint(rec, pos)
        elif tag == 5:
            level, pos = read_varint(rec, pos)
            key, pos = read_slice(rec, pos)
            edit["compact_pointers"].append((level, key))
        elif tag == 6:
            level, pos = read_varint(rec, pos)
            number, pos = read_varint(rec, pos)
            edit["deleted_files"].append((level, number))
        elif tag == 7:
            level, pos = read_varint(rec, pos)
            number, pos = read_varint(rec, pos)
            size, pos = read_varint(rec, pos)
            smallest, pos = read_slice(rec, pos)
            largest, pos = read_slice(rec, pos)
            edit["new_files"].append((level, number, size, smallest, largest))
        elif tag == 9:
            edit["prev_log_number"], pos = read_varint(rec, pos)
        else:
            raise ManifestError("unknown VersionEdit tag %d" % tag)
    return edit


def internal_user_key(ikey):
    if len(ikey) < 8:
        raise ManifestError("internal key too short")
    return ikey[:-8]


def internal_seq(ikey):
    return struct.unpack("<Q", ikey[-8:])[0] >> 8


def internal_key_less(a, b):
    """InternalKeyComparator order: user key ascending, then sequence descending."""
    ua, ub = internal_user_key(a), internal_user_key(b)
    if ua != ub:
        return ua < ub
    return internal_seq(a) > internal_seq(b)


class Replay:
    """Replays the edits of a manifest and records the layout after each."""

    def __init__(self, num_levels=7):
        self.num_levels = num_levels
        self.levels = [dict() for _ in range(num_levels)]  # number -> (size, smallest, largest)
        self.max_l0 = 0
        self.max_files = 0
        self.max_file_size = 0
        self.max_level_used = 0
        self.edits = 0
        self.added = 0
        self.deleted = 0
        self.added_bytes = 0
        self.log_number = None
        self.prev_log_number = None
        self.next_file_number = None
        self.last_sequence = None
        self.comparator = None
        self.history = []  # per edit: tuple of files per level
        self.bytes_history = []  # per edit: total table bytes
        self.max_total_bytes = 0
        self.max_l0_depth = 0
        self.memtable_switches = 0  # edits that moved to a new WAL (one per flush)

    def apply(self, edit):
        self.edits += 1
        if "comparator" in edit:
            self.comparator = edit["comparator"]
        if "log_number" in edit:
            if self.log_number is not None and edit["log_number"] != self.log_number:
                self.memtable_switches += 1
            self.log_number = edit["log_number"]
        if "prev_log_number" in edit:
            self.prev_log_number = edit["prev_log_number"]
        if "next_file_number" in edit:
            self.next_file_number = edit["next_file_number"]
        if "last_sequence" in edit:
            self.last_sequence = edit["last_sequence"]
        for level, number in edit["deleted_files"]:
            if level >= self.num_levels:
                raise ManifestError("deleted file at level %d" % level)
            if number not in self.levels[level]:
                raise ManifestError("edit deletes #%d from level %d where it is not" % (number, level))
            del self.levels[level][number]
            self.deleted += 1
        for level, number, size, smallest, largest in edit["new_files"]:
            if level >= self.num_levels:
                raise ManifestError("new file at level %d" % level)
            for lv in range(self.num_levels):
                if number in self.levels[lv]:
                    raise ManifestError("file #%d added twice" % number)
            self.levels[level][number] = (size, smallest, largest)
            self.added += 1
            self.added_bytes += size
            self.max_file_size = max(self.max_file_size, size)
            self.max_level_used = max(self.max_level_used, level)
        counts = tuple(len(l) for l in self.levels)
        self.history.append(counts)
        total = sum(f[0] for lv in self.levels for f in lv.values())
        self.bytes_history.append(total)
        self.max_total_bytes = max(self.max_total_bytes, total)
        self.max_l0 = max(self.max_l0, counts[0])
        self.max_l0_depth = max(self.max_l0_depth, self.l0_depth())
        self.max_files = max(self.max_files, sum(counts))

    def l0_depth(self):
        """Largest number of level-0 files whose key ranges share a point:
        the number of level-0 tables a point lookup may have to consult."""
        files = list(self.levels[0].values())
        if len(files) <= 1:
            return len(files)
        events = []
        for size, smallest, largest in files:
            events.append((internal_user_key(smallest), 0))
            events.append((internal_user_key(largest), 1))
        events.sort()
        depth = best = 0
        for _, kind in events:
            if kind == 0:
                depth += 1
                best = max(best, depth)
            else:
                depth -= 1
        return best

    def check_invariants(self):
        """Levels >= 1 hold non-overlapping files sorted by internal key
        (two neighbours may share a boundary user key with distinct
        sequence numbers, which the engine handles with AddBoundaryInputs)."""
        import functools
        for level in range(1, self.num_levels):
            files = sorted(self.levels[level].values(),
                           key=functools.cmp_to_key(
                               lambda x, y: -1 if internal_key_less(x[1], y[1]) else (1 if internal_key_less(y[1], x[1]) else 0)))
            for a, b in zip(files, files[1:]):
                if not internal_key_less(a[2], b[1]):
                    raise ManifestError("overlapping files at level %d" % level)

    def counts(self):
        return [len(l) for l in self.levels]

    def bytes_per_level(self):
        return [sum(f[0] for f in l.values()) for l in self.levels]

    def referenced(self):
        s = set()
        for l in self.levels:
            s.update(l.keys())
        return s


def parse_db(dbdir, verify_crc=True):
    """Parse CURRENT + MANIFEST of `dbdir`.  Returns (Replay, info dict)."""
    current_path = os.path.join(dbdir, "CURRENT")
    if not os.path.isfile(current_path):
        raise ManifestError("no CURRENT file")
    with open(current_path, "rb") as f:
        current = f.read()
    if not current.endswith(b"\n"):
        raise ManifestError("CURRENT does not end with newline")
    name = current[:-1].decode("ascii", "replace")
    if not re.match(r"^MANIFEST-\d+$", name):
        raise ManifestError("CURRENT names %r" % name)
    mpath = os.path.join(dbdir, name)
    if not os.path.isfile(mpath):
        raise ManifestError("manifest %s missing" % name)
    with open(mpath, "rb") as f:
        data = f.read()
    replay = Replay()
    records = read_log_records(data, verify_crc=verify_crc)
    if not records:
        raise ManifestError("manifest has no records")
    for rec in records:
        replay.apply(decode_edit(rec))
    replay.check_invariants()
    if replay.comparator != b"leveldb.BytewiseComparator":
        raise ManifestError("comparator %r" % replay.comparator)
    if replay.log_number is None or replay.next_file_number is None or replay.last_sequence is None:
        raise ManifestError("manifest lacks log/next-file/last-sequence")
    info = {"manifest": name, "manifest_bytes": len(data), "records": len(records)}
    return replay, info


def scan_dir(dbdir):
    """Classify the files present in a database directory."""
    out = {"tables": {}, "logs": {}, "manifests": [], "other": []}
    for name in os.listdir(dbdir):
        path = os.path.join(dbdir, name)
        m = re.match(r"^(\d+)\.(ldb|sst)$", name)
        if m:
            out["tables"][int(m.group(1))] = os.path.getsize(path)
            continue
        m = re.match(r"^(\d+)\.log$", name)
        if m:
            out["logs"][int(m.group(1))] = os.path.getsize(path)
            continue
        if name.startswith("MANIFEST-"):
            out["manifests"].append(name)
            continue
        if name in ("CURRENT", "LOCK", "LOG", "LOG.old") or name.endswith(".dbtmp"):
            out["other"].append(name)
            continue
        out["other"].append(name)
    return out


def summarize(dbdir):
    replay, info = parse_db(dbdir)
    present = scan_dir(dbdir)
    referenced = replay.referenced()
    info.update({
        "levels": replay.counts(),
        "level_bytes": replay.bytes_per_level(),
        "max_l0": replay.max_l0,
        "max_l0_depth": replay.max_l0_depth,
        "max_files": replay.max_files,
        "max_total_bytes": replay.max_total_bytes,
        "memtable_switches": replay.memtable_switches,
        "mean_total_bytes": int(sum(replay.bytes_history) / max(1, len(replay.bytes_history))),
        "max_file_size": replay.max_file_size,
        "max_level_used": replay.max_level_used,
        "edits": replay.edits,
        "files_added": replay.added,
        "files_deleted": replay.deleted,
        "bytes_added": replay.added_bytes,
        "log_number": replay.log_number,
        "last_sequence": replay.last_sequence,
        "next_file_number": replay.next_file_number,
        "tables_present": len(present["tables"]),
        "table_bytes_present": sum(present["tables"].values()),
        "log_bytes_present": sum(present["logs"].values()),
        "missing_tables": sorted(n for n in referenced if n not in present["tables"]),
        "orphan_tables": sorted(n for n in present["tables"] if n not in referenced),
        "orphan_bytes": sum(sz for n, sz in present["tables"].items() if n not in referenced),
        "referenced_bytes": sum(sz for n, sz in present["tables"].items() if n in referenced),
        "referenced_bytes_mismatch": sorted(
            n for n in referenced if n in present["tables"]
            and present["tables"][n] != [f for lv in replay.levels for k, f in lv.items() if k == n][0][0]),
        "stale_logs": sorted(n for n in present["logs"] if n < replay.log_number),
        "manifests_present": len(present["manifests"]),
    })
    return info


if __name__ == "__main__":
    import json
    import sys
    print(json.dumps(summarize(sys.argv[1]), indent=1, default=str))
