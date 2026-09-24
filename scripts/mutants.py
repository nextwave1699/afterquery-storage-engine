"""Small bugs injected into the reference (solution/reference.patch); the
conformance suite must fail each one.  name -> (file, old text, new text).
Used by docker_failure_paths.sh ("mutant:<name>") and README.md's table."""
M = {
    'iter-ignores-rangedel': ('db/db_iter.cc',
        '    if (ikey.type != kTypeDeletion && range_dels_ != nullptr &&',
        '    if (false && range_dels_ != nullptr &&'),
    'get-ignores-rangedel': ('db/lookup_state.h',
        '    if (type != kTypeDeletion && rd_ != nullptr &&',
        '    if (false && rd_ != nullptr &&'),
    'compaction-ignores-snapshots': ('db/db_impl.cc',
        '        if (e.type != kTypeDeletion && input_rd.Covers(user_key, e.seq, bound)) {',
        '        if (e.type != kTypeDeletion && input_rd.Covers(user_key, e.seq, kMaxSequenceNumber)) {'),
    'compaction-drops-tombstones': ('db/db_impl.cc',
        '    compact->tombstones.push_back(t);\n',
        ''),
    'iter-reversed-operands': ('db/db_iter.cc',
        '  std::vector<std::string> oldest_first(operands.rbegin(), operands.rend());',
        '  std::vector<std::string> oldest_first(operands.begin(), operands.end());'),
    'partial-merge-swapped': ('db/db_impl.cc',
        '          if (!merge_op->PartialMerge(user_key, acc, ops[k], &tmp)) {',
        '          if (!merge_op->PartialMerge(user_key, ops[k], acc, &tmp)) {'),
    'flush-drops-tombstones': ('db/db_impl.cc',
        '  mem->GetRangeTombstones(&tombstones);\n\n  Status s;',
        '\n  Status s;'),
    'merge-folds-across-snapshots': ('db/db_impl.cc',
        '    return std::lower_bound(snapshots.begin(), snapshots.end(), seq) -\n           snapshots.begin();',
        '    return 0;'),
    'memtable-get-stops-at-merge': ('db/memtable.cc',
        '    if (state->Add(static_cast<ValueType>(tag & 0xff), tag >> 8, v)) {\n      return true;\n    }',
        '    state->Add(static_cast<ValueType>(tag & 0xff), tag >> 8, v);\n    return true;'),
    'skip-ignores-layers': ('db/range_del.cc',
        '    return view_->CoveredRange(ikey.user_key, snapshot_, layer_, begin, end);',
        '    return view_->CoveredRange(ikey.user_key, snapshot_, kAllLayers, begin, end);'),
    'get-skip-ignores-layers': ('db/version_set.cc',
        '                                 state->version->LayerOf(level, f)) > 0) {',
        '                                 kAllLayers) > 0) {'),
    'skip-backward-no-prev': ('db/range_del.cc',
        '      if (iter_->Valid()) {\n        iter_->Prev();\n      } else {',
        '      if (iter_->Valid()) {\n      } else {'),
    'memget-skip-ignores-layers': ('db/db_impl.cc',
        '          range_dels->MaxCovering(key, snapshot, layer) > 0) {',
        '          range_dels->MaxCovering(key, snapshot) > 0) {'),
    'rdmap-split-drops-seqs': ('db/range_del.cc',
        '      Frag right = it->second;\n      it->second.end = stop;',
        '      Frag right;\n      right.end = it->second.end;\n      it->second.end = stop;'),
    'cf-log-never-trimmed': ('db/db_impl.cc',
        """    const uint64_t oldest = OldestLiveLog();
    if (logfile_number_ > oldest + kMaxLiveLogs) {""",
        """    const uint64_t oldest = OldestLiveLog();
    if (false) {"""),
    'cf-flush-claims-current-log': ('db/db_impl.cc',
        """    edit.SetLogNumber(cf->mem_log_number);""",
        """    edit.SetLogNumber(logfile_number_);"""),
    'cf-drop-not-durable': ('db/db_impl.cc',
        """  s = WriteFamilies(env_, dbname_, on_disk, next_family_id_);
  if (!s.ok()) return s;

  // Its files are nobody's now.""",
        """  // (the drop is not recorded)

  // Its files are nobody's now."""),
    'cf-recovery-applies-per-flush': ('db/db_impl.cc',
        """        status = WriteLevel0Table(cf, it->second, &recovery_edits_[cf->id],
                                  nullptr);""",
        """        status = WriteLevel0Table(cf, it->second, &recovery_edits_[cf->id],
                                  nullptr);
        if (status.ok()) {
          VersionEdit e = recovery_edits_[cf->id];
          e.SetLogNumber(log_number + 1);
          status = cf->versions->LogAndApply(&e, &mutex_);
          recovery_edits_.erase(cf->id);
        }"""),
}
