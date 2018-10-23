/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "PartitionedRocksDBStoreIterators.h"

#include "logdevice/common/debug.h"
#include "logdevice/server/locallogstore/RocksDBKeyFormat.h"

namespace facebook { namespace logdevice {

using RocksDBKeyFormat::PartitionDirectoryKey;
using Location = LocalLogStore::AllLogsIterator::Location;

// ==== Iterator ====

void PartitionedRocksDBStore::Iterator::setDataIteratorFromCurrent(
    ReadFilter* filter) {
  if (data_iterator_) {
    ld_check(data_iterator_->getCF() == current_.partition_->cf_.get());
  }
  if (!current_.partition_) {
    ld_check(!data_iterator_);
    return;
  }

  RecordTimestamp min_ts, max_ts;
  if (!filter || checkFilterTimeRange(*filter, &min_ts, &max_ts)) {
    if (data_iterator_ == nullptr) {
      data_iterator_ = std::make_unique<RocksDBLocalLogStore::CSIWrapper>(
          pstore_, log_id_, options_, current_.partition_->cf_.get());
    }
    data_iterator_->min_ts_ = min_ts;
    data_iterator_->max_ts_ = max_ts;
  } else {
    data_iterator_.reset();
  }
}

void PartitionedRocksDBStore::Iterator::setMetaIteratorAndCurrentFromLSN(
    lsn_t lsn) {
  if (data_iterator_) {
    ld_check(data_iterator_->getCF() == current_.partition_->cf_.get());
  }
  ld_check(latest_.partition_ != nullptr);

  createMetaIteratorIfNull();

  PartitionInfo new_current;

  STAT_INCR(pstore_->stats_, logsdb_iterator_dir_seek_needed);
  int rv = pstore_->findPartition(
      &*meta_iterator_, log_id_, lsn, &new_current.partition_);
  if (rv != 0) {
    if (err == E::NOTFOUND) {
      handleEmptyLog();
    } else {
      // meta_iterator_ has error or wouldblock.
      data_iterator_.reset();
      current_.clear();
    }
    return;
  }

  ld_check(meta_iterator_->Valid());
  ld_check(!meta_iterator_->status().IsIncomplete());
  ld_check(PartitionDirectoryKey::valid(
      meta_iterator_->key().data(), meta_iterator_->key().size()));
  ld_check_eq(
      PartitionDirectoryKey::getLogID(meta_iterator_->key().data()), log_id_);
  ld_check_eq(PartitionDirectoryKey::getPartition(meta_iterator_->key().data()),
              new_current.partition_->id_);
  checkDirectoryValue();

  new_current.min_lsn_ =
      PartitionDirectoryKey::getLSN(meta_iterator_->key().data());
  new_current.max_lsn_ = PartitionDirectoryValue::getMaxLSN(
      meta_iterator_->value().data(), meta_iterator_->value().size());

  // Note that we compare partition_ pointers here, not partition IDs.
  // That's because it's possible that the partition
  // was dropped and another one was created with the same ID.
  if (new_current.partition_ != current_.partition_) {
    // Need to do this before assigning current to make sure CF handle outlives
    // iterator.
    data_iterator_.reset();
  }

  current_ = new_current;
}

bool PartitionedRocksDBStore::Iterator::setMetaIteratorFromCurrent() {
  ld_check(current_.partition_);
  STAT_INCR(pstore_->stats_, logsdb_iterator_dir_reseek_needed);
  createMetaIteratorIfNull();

  PartitionDirectoryKey key(
      log_id_, current_.min_lsn_, current_.partition_->id_);
  // SeekForPrev instead of Seek for two reasons:
  //  (a) min_lsn may have been decreased since current_ was filled out,
  //  (b) setMetaIteratorFromCurrent() is only used when iterating backwards,
  //      so seeking backwards avoids reversing direction (a slightly slow
  //      operation).
  meta_iterator_->SeekForPrev(
      rocksdb::Slice(reinterpret_cast<const char*>(&key), sizeof(key)));
  if (!meta_iterator_->status().ok()) {
    return false;
  }

  if (meta_iterator_->Valid() && meta_iterator_->status().ok()) {
    rocksdb::Slice key = meta_iterator_->key();
    if (PartitionDirectoryKey::valid(key.data(), key.size()) &&
        PartitionDirectoryKey::getLogID(key.data()) == log_id_ &&
        PartitionDirectoryKey::getPartition(key.data()) ==
            current_.partition_->id_) {
      return true;
    }
  }

  STAT_INCR(pstore_->stats_, logsdb_iterator_partition_dropped);
  return false;
}

void PartitionedRocksDBStore::Iterator::setCurrent(PartitionInfo new_current) {
  if (data_iterator_) {
    ld_check(data_iterator_->getCF() == current_.partition_->cf_.get());
  }
  ld_check(new_current.partition_ != nullptr);

  if (new_current.partition_ != current_.partition_) {
    data_iterator_.reset();
  }

  current_ = new_current;
}

void PartitionedRocksDBStore::Iterator::checkDirectoryValue() const {
  dd_assert(
      PartitionDirectoryValue::valid(
          meta_iterator_->value().data(), meta_iterator_->value().size()),
      "Invalid directory value %s for key %s",
      hexdump_buf(
          meta_iterator_->value().data(), meta_iterator_->value().size())
          .c_str(),
      hexdump_buf(meta_iterator_->key().data(), meta_iterator_->key().size())
          .c_str());
}

void PartitionedRocksDBStore::Iterator::checkAccessedUnderReplicatedRegion(
    PartitionInfo start,
    PartitionInfo end,
    Operation op,
    lsn_t seek_lsn) {
  ld_spew("pstart: %s, pend: %s, op: %s, seek_lsn %s",
          logdevice::toString(start).c_str(),
          logdevice::toString(end).c_str(),
          toString(op).c_str(),
          lsn_to_string(seek_lsn).c_str());

  if (!pstore_->isUnderReplicated()) {
    ld_spew("FALSE: Nothing under-replicated");
    accessed_underreplicated_region = false;
    return;
  }

  if (accessed_underreplicated_region) {
    // We've already touched an under-replicated region.
    // No point in checking again.
    ld_spew("TRUE: already under-replicated");
    return;
  }

  // Three cases are handled differently:
  //  1. Seek/SeekForPrev.
  //  2. Next/Prev that went out of bounds.
  //  3. Next/Prev that ended up on a record.

  // 1.
  if (op == Operation::SEEK || op == Operation::SEEK_FOR_PREV) {
    ld_check_eq(start.partition_, end.partition_);
    ld_check(op == Operation::SEEK || op == Operation::SEEK_FOR_PREV);

    if (!end.partition_) {
      // Seek didn't find any records.
      // Assume under replication anywhere in the store applies to this seek.
      ld_spew("TRUE: seek didn't find any records");
      accessed_underreplicated_region = true;
      return;
    }

    if (end.partition_->dirty_state_.under_replicated) {
      // Landed in an under-replicated partition.
      ld_spew("TRUE: landed in under-replicated partition");
      accessed_underreplicated_region = true;
      return;
    }

    // On initial seek, if we land in a fully-replicated partition
    // the outcome of the operation may still have been impacted by
    // under replication of partitions outside the current partition.
    // For seek operations, if the min_lsn of this partition is <= to
    // the initial seek point, we haven't skipped over any potentially
    // missing records. For seekForPrev() operations, if the max_lsn of
    // the partition is >= to the initial seek point, again we can assume
    // no records have been missed.  Otherwise, be extremely conservative
    // and assume that an under-replicated partitions before or after
    // (depending on seek direction) the current partition may have lost
    // records that would have yielded a more accurate seek result.
    switch (op) {
      case Operation::SEEK:
        if (end.min_lsn_ > seek_lsn &&
            pstore_->min_under_replicated_partition_ < end.partition_->id_) {
          ld_spew("SEEK: true");
          accessed_underreplicated_region = true;
          return;
        }
        ld_spew("SEEK: false");
        break;
      case Operation::SEEK_FOR_PREV:
        if (end.max_lsn_ < seek_lsn &&
            pstore_->max_under_replicated_partition_ > end.partition_->id_) {
          ld_spew("SEEK_FOR_PREV: true");
          accessed_underreplicated_region = true;
          return;
        }
        ld_spew("SEEK_FOR_PREV: false");
        break;
      default:
        ld_check(false);
    }

    return;
  }

  ld_check(start.partition_);

  // 2.
  if (state() == IteratorState::AT_END || !end.partition_) {
    // If the iterator is at the end of the log, any under-replicated
    // partition beyond where the iterator started could have lost records.
    if (op == Operation::NEXT) {
      if (start.partition_->id_ <= pstore_->max_under_replicated_partition_) {
        ld_spew("AT_END NEXT: true");
        accessed_underreplicated_region = true;
        return;
      }
      ld_spew("AT_END NEXT: false");
    } else if (op == Operation::PREV) {
      if (end.partition_->id_ >= pstore_->min_under_replicated_partition_) {
        accessed_underreplicated_region = true;
        ld_spew("AT_END PREV: true");
        return;
      }
      ld_spew("AT_END PREV: false");
    } else {
      ld_check(false);
    }
    return;
  }

  // 3.
  partition_id_t low = start.partition_->id_;
  partition_id_t high = end.partition_->id_;
  if (low > high) {
    ld_check(op == Operation::PREV);
    std::swap(low, high);
  }

  // Constrain range to known under-replicated space.
  low = std::max(
      low,
      pstore_->min_under_replicated_partition_.load(std::memory_order_relaxed));
  high = 1 +
      std::min(high - 1,
               pstore_->max_under_replicated_partition_.load(
                   std::memory_order_relaxed));

  if (low >= high) {
    ld_spew("SCAN: false (empty range)");
    return;
  }

  auto partitions = pstore_->getPartitionList();
  for (partition_id_t i = low; i <= high; ++i) {
    PartitionPtr p = partitions->get(i);
    if (p && p->dirty_state_.under_replicated) {
      accessed_underreplicated_region = true;
      ld_spew("SCAN: true");
      break;
    }
  }
  ld_spew("SCAN: false");
}

bool PartitionedRocksDBStore::Iterator::checkFilterTimeRange(
    ReadFilter& filter,
    RecordTimestamp* out_min_ts,
    RecordTimestamp* out_max_ts) {
  ld_check(out_min_ts != nullptr);
  ld_check(out_max_ts != nullptr);
  *out_min_ts = current_.partition_->min_timestamp;
  *out_max_ts = current_.partition_->max_timestamp;
  return filter.shouldProcessTimeRange(*out_min_ts, *out_max_ts);
}

void PartitionedRocksDBStore::Iterator::
    assertDataIteratorHasCorrectTimeRange() {
  ld_check(data_iterator_->min_ts_ >= current_.partition_->min_timestamp &&
           data_iterator_->max_ts_ <= current_.partition_->max_timestamp);
}

void PartitionedRocksDBStore::Iterator::createMetaIteratorIfNull() {
  if (meta_iterator_.hasValue()) {
    return;
  }

  meta_iterator_ = pstore_->createMetadataIterator(options_.allow_blocking_io);
}

void PartitionedRocksDBStore::Iterator::updatePartitionRange() {
  // Get latest partition and its first LSN.
  PartitionInfo new_latest;
  auto it = pstore_->logs_.find(log_id_.val_);
  if (it != pstore_->logs_.cend()) {
    const PartitionedRocksDBStore::LogState* log_state = it->second.get();
    partition_id_t id;
    lsn_t min_lsn;
    lsn_t max_lsn;
    log_state->latest_partition.load(&id, &min_lsn, &max_lsn);
    // If getPartition() fails, it means that the partition `id` was already
    // dropped; consider the log empty in this case.
    if (id != PARTITION_INVALID &&
        pstore_->getPartition(id, &new_latest.partition_)) {
      new_latest.min_lsn_ = min_lsn;
      new_latest.max_lsn_ = max_lsn;
    }
  } else {
    // Log has no directory entry so no data. Using a default PartitionInfo
    // (null partition_ pointer).
  }

  partition_id_t oldest_partition_id = pstore_->oldest_partition_id_.load();

  latest_ = new_latest;
  oldest_partition_id_ = oldest_partition_id;

  if (latest_.partition_ == nullptr) {
    handleEmptyLog();
  }
}

void PartitionedRocksDBStore::Iterator::handleEmptyLog() {
  // Assume under replication anywhere in the store applies to this log too.
  accessed_underreplicated_region |= pstore_->isUnderReplicated();
  // Destruction order matters (CF handle should outlive iterator).
  data_iterator_.reset();
  current_.clear();
  latest_.clear();
  meta_iterator_.clear();
  state_ = IteratorState::AT_END;
}

void PartitionedRocksDBStore::Iterator::moveUntilValid(bool forward,
                                                       lsn_t current_lsn,
                                                       ReadFilter* filter,
                                                       ReadStats* it_stats) {
  if (filter || it_stats) {
    // Filtering args are only compatible with moving forward
    ld_check(forward);
  }
  state_ = IteratorState::MAX;

  // Returns true if the data iterator is pointing to an LSN that exceeds
  // the max LSN of the current partiion. This can occur when partition
  // directory updates are lost due to a crash. When the record is later
  // re-replicated by recovery or rebuildling, the record may be stored
  // in a different partition. To avoid exposing this inconsistency
  // (e.g. duplicate records, records with same LSN but different contexts
  // - data vs. bridge, etc.) we skip orphaned data records.
  //
  // NOTE: atOrphanedRecord() can also return true for records written
  //       after the metadata iterator was last refreshed. The iterator
  //       contract is that new records are guaranteed to be visible
  //       only if this is a tailing iterator and a seek has been performed
  //       since the records were written. So, incorrectly treating new
  //       records as orphans here is ok.
  auto atOrphanedRecord = [&] {
    ld_check_ne(current_.max_lsn_, LSN_INVALID);
    return (getLSN() > current_.max_lsn_);
  };

  // seekForPrev() and seekToLast() already take orphaned records
  // into account. prev() can only be applied when iterator is
  // already positioned on a valid record.
  ld_assert(forward || data_iterator_->state() != IteratorState::AT_RECORD ||
            !atOrphanedRecord());

  SCOPE_EXIT {
    ld_check(state_ != IteratorState::MAX);
    if (data_iterator_ == nullptr && current_.partition_ != nullptr) {
      // All partitions got filtered out by ReadFilter.
      ld_check_eq(state_, IteratorState::AT_END);
      ld_check(filter);
      current_.clear();
    }
  };

  // Move data_iterator_ from partition to partition until we find a good
  // record, or run out of partitions, or encounter an error.
  while (true) {
    // See if we're already in a good state.

    if (data_iterator_) {
      IteratorState s = data_iterator_->state();
      if (s == IteratorState::AT_RECORD || s == IteratorState::LIMIT_REACHED) {
        if (!atOrphanedRecord()) {
          // Found a good record.
          state_ = s;
          return;
        }
      } else if (s != IteratorState::AT_END) {
        ld_check_in(s, ({IteratorState::ERROR, IteratorState::WOULDBLOCK}));
        state_ = s;
        return;
      }
    }

    // Need to move to the next/prev partition. See if there's nowhere to move.

    if (current_.partition_ != nullptr &&
        ((forward && current_.partition_->id_ >= latest_.partition_->id_) ||
         (!forward && current_.partition_->id_ <= oldest_partition_id_))) {
      // Nowhere to move.
      // Note that we don't destroy data_iterator_ here because, for
      // CatchupOneStream use case, it's likely that the next seek
      // will go to the same partition.
      state_ = IteratorState::AT_END;
      return;
    }

    // Initialize meta_iterator_ if needed.

    bool meta_iterator_is_at_prev = false;
    if (!meta_iterator_.hasValue()) {
      // meta_iterator_ can be unset only if current_ == latest_, in which case
      // we'd hit the `if` above.
      ld_check(!forward);

      meta_iterator_is_at_prev = !setMetaIteratorFromCurrent();
    }

    // Move meta_iterator_ and see if it still points to a directory entry.

    if (forward) {
      meta_iterator_->Next();
    } else if (!meta_iterator_is_at_prev) {
      meta_iterator_->Prev();
    }

    if (!meta_iterator_->Valid() || !meta_iterator_->status().ok()) {
      state_ = IteratorState::AT_END;
      return;
    }

    rocksdb::Slice key = meta_iterator_->key();

    if (!PartitionDirectoryKey::valid(key.data(), key.size()) ||
        PartitionDirectoryKey::getLogID(key.data()) != log_id_) {
      // no more entries for `log_id_' in the directory, abort
      state_ = IteratorState::AT_END;
      return;
    }

    checkDirectoryValue();
    auto partition_id = PartitionDirectoryKey::getPartition(key.data());
    if (current_.partition_ != nullptr &&
        partition_id == current_.partition_->id_) {
      // This should be impossible.
      RATELIMIT_ERROR(std::chrono::seconds(10),
                      10,
                      "Multiple directory entries for partition %lu, log %lu",
                      partition_id,
                      log_id_.val_);
      data_iterator_.reset();
      current_.clear();
      continue;
    }

    // Look up Partition by ID and point data_iterator_ to it.

    {
      PartitionInfo new_current;

      if (!pstore_->getPartition(partition_id, &new_current.partition_)) {
        // Partition dropped.
        STAT_INCR(pstore_->stats_, logsdb_iterator_partition_dropped);
        if (forward) {
          // Make sure meta_iterator_ and current_ don't end up pointing
          // to different partitions.
          data_iterator_.reset();
          current_.clear();
          continue;
        } else {
          state_ = IteratorState::AT_END;
          return;
        }
      }

      new_current.min_lsn_ = PartitionDirectoryKey::getLSN(key.data());
      new_current.max_lsn_ = PartitionDirectoryValue::getMaxLSN(
          meta_iterator_->value().data(), meta_iterator_->value().size());

      // Set new_current as current_.
      setCurrent(new_current);
      setDataIteratorFromCurrent(filter);
    }

    if (it_stats) {
      ++it_stats->seen_logsdb_partitions;
    }

    // Seek the new data_iterator_.

    if (forward) {
      // We just moved to the next partition. Essentially we need to seek to
      // the first record of this partition. But instead we'll seek to
      // max(current_lsn, min_lsn_), which is usually the same thing since
      // current_lsn is usually in previous partition, and min_lsn_ is the first
      // lsn in this partition. However, it's important because:
      //  * If data_iterator_ reaches read limit before seeing any records
      //    (in particular, if it reaches stop_reading_after_timestamp
      //    immediately, which is often the case in rebuilding), it'll report
      //    the seek lsn as getLSN() at which it stopped. If we filter out lots
      //    of records in previous partitions and then seek to a small lsn in
      //    this partition, our getLSN() will be small, i.e. and all the
      //    filtering progress will be lost.
      //  * If this partition for some reason has records with lsn smaller than
      //    in previous partitions (which should be impossible currently),
      //    we'd like to skip these records rather than presenting a decreasing
      //    sequence of lsns to the user of iterator.
      current_lsn = std::max(current_lsn, current_.min_lsn_);

      if (filter) {
        ld_check(it_stats);
        it_stats->max_read_timestamp_lower_bound =
            current_.partition_->starting_timestamp.toMilliseconds();

        if (data_iterator_) {
          // seek to the smallest key >= current_lsn in the new partition
          // that passes the filter
          assertDataIteratorHasCorrectTimeRange();
          data_iterator_->seek(current_lsn, filter, it_stats);
        }
      } else {
        // seek to the smallest key >= current_lsn in the new partition if
        // there is no filter
        data_iterator_->seek(current_lsn);
      }
    } else {
      // Skip any orphaned records.
      current_lsn = std::min(current_lsn, current_.max_lsn_);
      data_iterator_->seekForPrev(current_lsn);
    }
  }
}

void PartitionedRocksDBStore::Iterator::seek(lsn_t lsn,
                                             ReadFilter* filter,
                                             ReadStats* stats) {
  trackSeek(lsn, 0);

  // Reset sticky state on seeks.
  accessed_underreplicated_region = false;

  updatePartitionRange();
  if (!latest_.partition_) {
    // Log is empty.
    return;
  }

  if (lsn >= latest_.min_lsn_) {
    // The fast path -- lsn belongs to the latest partition.
    // Don't need meta_iterator_.
    ld_check(latest_.min_lsn_ != LSN_INVALID);
    meta_iterator_.clear();
    setCurrent(latest_);
  } else {
    // Slow path: seek meta_iterator_ in directory, then data_iterator_ in
    // partition.

    // Calling refresh on the metadata iterator to pick up the latest updates
    // and to unpin any stale memtable that this meta_iterator could have been
    // pinning. Note: Cost of calling refresh when nothing has changed (i.e. no
    // memtables flushed) is assumed to be negligible. Also note that only
    // tailing iterators are expected to be refreshed (i.e. see new data and
    // unpin memtables and files) on every seek*().
    if (options_.tailing && meta_iterator_.hasValue()) {
      meta_iterator_->Refresh(); // also clears status()
    }

    setMetaIteratorAndCurrentFromLSN(lsn);

    if (!current_.partition_) {
      // setMetaIteratorAndCurrentFromLSN() saw the log as empty, but
      // updatePartitionRange() saw it as nonempty. This is unlikely.
      return;
    }
  }

  setDataIteratorFromCurrent(filter);

  if (data_iterator_) {
    if (filter) {
      ld_check(stats);
      ++stats->seen_logsdb_partitions;
      assertDataIteratorHasCorrectTimeRange();
    }
    data_iterator_->seek(lsn, filter, stats);
  }
  moveUntilValid(true, lsn, filter, stats);
  checkAccessedUnderReplicatedRegion(current_, current_, Operation::SEEK, lsn);
  ld_check(state() != IteratorState::LIMIT_REACHED ||
           (stats && stats->readLimitReached()));
}

void PartitionedRocksDBStore::Iterator::seekForPrev(lsn_t lsn) {
  trackSeek(lsn, 0);

  // Reset sticky state on seeks.
  accessed_underreplicated_region = false;

  updatePartitionRange();
  if (!latest_.partition_) {
    return;
  }

  if (lsn >= latest_.min_lsn_) {
    // The fast path -- lsn belongs to the latest partition.
    // Don't need meta_iterator_.
    ld_check(latest_.min_lsn_ != LSN_INVALID);
    meta_iterator_.clear();
    setCurrent(latest_);
    setDataIteratorFromCurrent();
    ld_check(data_iterator_ != nullptr);
    // Skip any orphaned records.
    data_iterator_->seekForPrev(std::min(lsn, current_.max_lsn_));
    IteratorState s = data_iterator_->state();
    if (s != IteratorState::AT_END) {
      ld_check_in(s,
                  ({IteratorState::AT_RECORD,
                    IteratorState::ERROR,
                    IteratorState::WOULDBLOCK}));
      checkAccessedUnderReplicatedRegion(
          current_, current_, Operation::SEEK_FOR_PREV, lsn);
      state_ = s;
      return;
    } else {
      // No matching records in latest partition. Fall back to slow path.
      // This will needlessly create and seek data_iterator_ in latest patition
      // again, which is suboptimal. Feel free to optimize if it turns out to
      // be a problem.
    }
  }

  // find which partition lsn belongs to
  setMetaIteratorAndCurrentFromLSN(lsn);
  if (!current_.partition_) {
    // Log is empty.
    return;
  }
  setDataIteratorFromCurrent();
  ld_check(data_iterator_ != nullptr);

  lsn_t max_valid_lsn = std::min(lsn, current_.max_lsn_);
  data_iterator_->seekForPrev(max_valid_lsn);
  moveUntilValid(false, max_valid_lsn);
  checkAccessedUnderReplicatedRegion(
      current_, current_, Operation::SEEK_FOR_PREV, lsn);
}

void PartitionedRocksDBStore::Iterator::next(ReadFilter* filter,
                                             ReadStats* stats) {
  ld_check_eq(state(), IteratorState::AT_RECORD);

  PartitionInfo start = current_;
  lsn_t current_lsn = data_iterator_->getLSN();
  if (filter) {
    assertDataIteratorHasCorrectTimeRange();
  }
  data_iterator_->next(filter, stats);
  moveUntilValid(true, std::min(current_lsn, LSN_MAX - 1) + 1, filter, stats);
  ld_check(state() != IteratorState::LIMIT_REACHED ||
           (stats && stats->readLimitReached()));
  checkAccessedUnderReplicatedRegion(start, current_, Operation::NEXT);
}

void PartitionedRocksDBStore::Iterator::prev() {
  ld_assert(state() == IteratorState::AT_RECORD);

  PartitionInfo start = current_;
  lsn_t current_lsn = data_iterator_->getLSN();
  data_iterator_->prev();
  moveUntilValid(false, std::max(1ul, current_lsn) - 1);
  checkAccessedUnderReplicatedRegion(start, current_, Operation::PREV);
}

IteratorState PartitionedRocksDBStore::Iterator::state() const {
  if (meta_iterator_.hasValue() && !meta_iterator_->status().ok()) {
    return meta_iterator_->status().IsIncomplete() ? IteratorState::WOULDBLOCK
                                                   : IteratorState::ERROR;
  }
  ld_check(state_ != IteratorState::MAX);
  if (state_ == IteratorState::AT_RECORD ||
      state_ == IteratorState::LIMIT_REACHED) {
    ld_check(data_iterator_ != nullptr);
    ld_check_eq(data_iterator_->state(), state_);
  }
  return state_;
}

lsn_t PartitionedRocksDBStore::Iterator::getLSN() const {
  ld_check(data_iterator_ != nullptr);
  return data_iterator_->getLSN();
}

Slice PartitionedRocksDBStore::Iterator::getRecord() const {
  ld_check(data_iterator_ != nullptr);
  return data_iterator_->getRecord();
}

void PartitionedRocksDBStore::Iterator::seekToPartitionBeforeOrAfter(
    lsn_t lsn,
    partition_id_t partition_id,
    bool after) {
  ld_check(options_.allow_blocking_io);
  updatePartitionRange();
  if (latest_.partition_ == nullptr) {
    return;
  }
  createMetaIteratorIfNull();

  // Seek to the given partition (or a later one if it was dropped).
  PartitionDirectoryKey key(log_id_, lsn, partition_id);
  if (after) {
    meta_iterator_->Seek(
        rocksdb::Slice(reinterpret_cast<const char*>(&key), sizeof(key)));
  } else {
    meta_iterator_->SeekForPrev(
        rocksdb::Slice(reinterpret_cast<const char*>(&key), sizeof(key)));
  }

  // Move to a non-dropped partition.
  PartitionInfo new_current;
  while (true) {
    if (!meta_iterator_->Valid() || !meta_iterator_->status().ok()) {
      state_ = IteratorState::AT_END;
      return;
    }
    rocksdb::Slice key_slice = meta_iterator_->key();
    if (!PartitionDirectoryKey::valid(key_slice.data(), key_slice.size()) ||
        PartitionDirectoryKey::getLogID(key_slice.data()) != log_id_) {
      state_ = IteratorState::AT_END;
      return;
    }
    checkDirectoryValue();
    partition_id_t id = PartitionDirectoryKey::getPartition(key_slice.data());
    if ((after ? (id > partition_id) : (id < partition_id)) &&
        pstore_->getPartition(id, &new_current.partition_)) {
      new_current.min_lsn_ = PartitionDirectoryKey::getLSN(key_slice.data());
      new_current.max_lsn_ = PartitionDirectoryValue::getMaxLSN(
          meta_iterator_->value().data(), meta_iterator_->value().size());
      break;
    }
    if (!after && id < partition_id) {
      // If this partition is dropped, no need to go further back - everything
      // is dropped there too.
      state_ = IteratorState::AT_END;
      return;
    }
    if (after) {
      meta_iterator_->Next();
    } else {
      meta_iterator_->Prev();
    }
  }

  // Create and seek data iterator.
  setCurrent(new_current);
  setDataIteratorFromCurrent();
  if (after) {
    data_iterator_->seek(lsn_t(0));
  } else {
    data_iterator_->seekForPrev(current_.max_lsn_);
  }
  moveUntilValid(after, after ? lsn_t(0) : LSN_MAX);
}

// ==== PartitionedAllLogsIterator ====

PartitionedRocksDBStore::PartitionedAllLogsIterator::PartitionedAllLogsIterator(
    const PartitionedRocksDBStore* pstore,
    const LocalLogStore::ReadOptions& options)
    : pstore_(pstore),
      options_(options),
      last_partition_id_(pstore->getLatestPartition()->id_) {
  ld_check(!options.tailing);
  registerTracking(std::string(),
                   LOGID_INVALID,
                   /* tailing */ false,
                   options.allow_blocking_io,
                   IteratorType::PARTITIONED_ALL_LOGS,
                   options.tracking_ctx);
}

IteratorState
PartitionedRocksDBStore::PartitionedAllLogsIterator::state() const {
  if (!data_iterator_) {
    return IteratorState::AT_END;
  }
  IteratorState s = data_iterator_->state();
  ld_check(s != IteratorState::AT_END);
  return s;
}

logid_t PartitionedRocksDBStore::PartitionedAllLogsIterator::getLogID() const {
  ld_check(data_iterator_ != nullptr);
  return data_iterator_->getLogID();
}
lsn_t PartitionedRocksDBStore::PartitionedAllLogsIterator::getLSN() const {
  ld_check(data_iterator_ != nullptr);
  return data_iterator_->getLSN();
}
Slice PartitionedRocksDBStore::PartitionedAllLogsIterator::getRecord() const {
  ld_check(data_iterator_ != nullptr);
  return data_iterator_->getRecord();
}
std::unique_ptr<Location>
PartitionedRocksDBStore::PartitionedAllLogsIterator::getLocation() const {
  ld_check_in(
      state(), ({IteratorState::AT_RECORD, IteratorState::LIMIT_REACHED}));
  return std::make_unique<PartitionedLocation>(
      current_partition_ ? current_partition_->id_ : PARTITION_INVALID,
      data_iterator_->getLogID(),
      data_iterator_->getLSN());
}

void PartitionedRocksDBStore::PartitionedAllLogsIterator::seek(
    const Location& base_location,
    ReadFilter* filter,
    ReadStats* stats) {
  PartitionedLocation location =
      checked_downcast<const PartitionedLocation&>(base_location);

  trackSeek(lsn_t(0), /* version */ 0);
  setPartition(location.partition, filter, stats);
  if (!data_iterator_) {
    return;
  }
  if (current_partition_ && current_partition_->id_ != location.partition) {
    ld_check_gt(current_partition_->id_, location.partition);
    location.log = logid_t(0);
    location.lsn = 0;
  }
  data_iterator_->seek(location.log, location.lsn, filter, stats);
  moveUntilValid(filter, stats);
}
void PartitionedRocksDBStore::PartitionedAllLogsIterator::next(
    ReadFilter* filter,
    ReadStats* stats) {
  ld_assert_eq(state(), IteratorState::AT_RECORD);
  data_iterator_->next(filter, stats);
  moveUntilValid(filter, stats);
}

std::unique_ptr<Location>
PartitionedRocksDBStore::PartitionedAllLogsIterator::minLocation() const {
  return std::make_unique<PartitionedLocation>(
      PARTITION_INVALID, logid_t(0), lsn_t(0));
}
std::unique_ptr<Location>
PartitionedRocksDBStore::PartitionedAllLogsIterator::metadataLogsBegin() const {
  return std::make_unique<PartitionedLocation>(
      PARTITION_INVALID, MetaDataLog::metaDataLogID(logid_t(0)), lsn_t(0));
}

void PartitionedRocksDBStore::PartitionedAllLogsIterator::invalidate() {
  trackIteratorRelease();
  current_partition_ = nullptr;
  data_iterator_ = nullptr;
}

void PartitionedRocksDBStore::PartitionedAllLogsIterator::setPartition(
    partition_id_t partition,
    ReadFilter* filter,
    ReadStats* stats) {
  SCOPE_EXIT {
    ld_check(!current_partition_ || data_iterator_);
  };

  data_iterator_ = nullptr;
  auto partitions = pstore_->getPartitionList();
  RecordTimestamp min_ts;
  RecordTimestamp max_ts;

  // Iterate over partitions until we see one that `filter` accepts.
  for (;; ++partition) {
    if (partition > last_partition_id_) {
      // We're at end.
      current_partition_ = nullptr;
      return;
    }

    if (partition == PARTITION_INVALID) {
      current_partition_ = nullptr;
      min_ts = RecordTimestamp::min();
      max_ts = RecordTimestamp::max();
    } else {
      // Latest partition ID never decreases.
      ld_assert_lt(partition, partitions->nextID());

      partition = std::max(partition, partitions->firstID());
      current_partition_ = partitions->get(partition);
      min_ts = current_partition_->min_timestamp;
      max_ts = current_partition_->max_timestamp;
    }
    if (!filter || filter->shouldProcessTimeRange(min_ts, max_ts)) {
      // Found a good partition.
      break;
    }
  }

  data_iterator_ = std::make_unique<RocksDBLocalLogStore::CSIWrapper>(
      pstore_,
      /* log_id */ folly::none,
      options_,
      current_partition_ ? current_partition_->cf_.get()
                         : pstore_->unpartitioned_cf_.get());
  data_iterator_->min_ts_ = min_ts;
  data_iterator_->max_ts_ = max_ts;

  if (stats) {
    ++stats->seen_logsdb_partitions;
  }
}

void PartitionedRocksDBStore::PartitionedAllLogsIterator::moveUntilValid(
    ReadFilter* filter,
    ReadStats* stats) {
  while (data_iterator_ && data_iterator_->state() == IteratorState::AT_END) {
    setPartition(
        current_partition_ ? current_partition_->id_ + 1 : 1, filter, stats);
    if (data_iterator_) {
      data_iterator_->seek(logid_t(0), lsn_t(0), filter, stats);
    }
  }
}

const LocalLogStore*
PartitionedRocksDBStore::PartitionedAllLogsIterator::getStore() const {
  return pstore_;
}
bool PartitionedRocksDBStore::PartitionedAllLogsIterator::tracingEnabled()
    const {
  return false;
}

// ==== PartitionDirectoryIterator ====

bool PartitionedRocksDBStore::PartitionDirectoryIterator::error() {
  return !meta_it_.status().ok();
}

bool PartitionedRocksDBStore::PartitionDirectoryIterator::nextLog() {
  while (true) {
    partition_id_ = PARTITION_INVALID;
    ++log_id_.val_;

    // Seek to the first entry for the first non-empty log >= log_id.
    PartitionDirectoryKey key(log_id_, lsn_t(0), partition_id_t(0));
    meta_it_.Seek(
        rocksdb::Slice(reinterpret_cast<const char*>(&key), sizeof(key)));

    if (!itValid()) {
      // No more logs.
      return false;
    }

    log_id_ = itLogID();

    // We are only interested in log if it appears in non-latest partition.
    if (PartitionDirectoryKey::getPartition(meta_it_.key().data()) <
        latest_partition_) {
      return true;
    }
  }
}

logid_t PartitionedRocksDBStore::PartitionDirectoryIterator::getLogID() {
  ld_check(log_id_ != LOGID_INVALID);
  return log_id_;
}

bool PartitionedRocksDBStore::PartitionDirectoryIterator::nextPartition() {
  ld_check(log_id_ != LOGID_INVALID);
  if (!itValid() || itLogID() != log_id_) {
    ld_check(partition_id_ != PARTITION_INVALID);
    partition_id_ = PARTITION_INVALID;
    return false;
  }

  partition_id_ = PartitionDirectoryKey::getPartition(meta_it_.key().data());

  if (partition_id_ >= latest_partition_) {
    // Skip latest partition.
    ld_check(partition_id_ != PARTITION_INVALID);
    partition_id_ = PARTITION_INVALID;
    return false;
  }

  first_lsn_ = PartitionDirectoryKey::getLSN(meta_it_.key().data());

  if (!dd_assert(
          PartitionDirectoryValue::valid(
              meta_it_.value().data(), meta_it_.value().size()),
          "Invalid directory value %s for key %s",
          hexdump_buf(meta_it_.value().data(), meta_it_.value().size()).c_str(),
          hexdump_buf(meta_it_.key().data(), meta_it_.key().size()).c_str())) {
    return false;
  }
  last_lsn_ = PartitionDirectoryValue::getMaxLSN(
      meta_it_.value().data(), meta_it_.value().size());

  meta_it_.Next();

  return true;
}
partition_id_t
PartitionedRocksDBStore::PartitionDirectoryIterator::getPartitionID() {
  ld_check(partition_id_ != PARTITION_INVALID);
  return partition_id_;
}

lsn_t PartitionedRocksDBStore::PartitionDirectoryIterator::getLastLSN() {
  ld_check(partition_id_ != PARTITION_INVALID);
  return last_lsn_;
}

bool PartitionedRocksDBStore::PartitionDirectoryIterator::itValid() {
  return meta_it_.status().ok() && meta_it_.Valid() &&
      PartitionDirectoryKey::valid(
             meta_it_.key().data(), meta_it_.key().size());
}

logid_t PartitionedRocksDBStore::PartitionDirectoryIterator::itLogID() {
  return PartitionDirectoryKey::getLogID(meta_it_.key().data());
}

}} // namespace facebook::logdevice
