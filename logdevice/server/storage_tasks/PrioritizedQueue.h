/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <folly/MPMCQueue.h>
#include <folly/SharedMutex.h>
#include <folly/small_vector.h>
#include <logdevice/common/Semaphore.h>
#include <logdevice/common/debug.h>
#include <logdevice/common/stats/Stats.h>
#include <logdevice/common/util.h>

/**
 * @file  The priority queue implementation used by StorageThreadPool.
 *        Strict priority can be relaxed by providing a yield schedule
 *        that causes priorities to be periodically masked from consideration
 *        during read attempts of the queue.
 */
namespace facebook { namespace logdevice {

template <class T, size_t NumPriorities>
class PrioritizedQueue {
 public:
  PrioritizedQueue(size_t size, StatsHolder* stats) : stats_(stats) {
    for (size_t i = 0; i < NumPriorities; ++i) {
      queues_.emplace_back(size);
    }
  }

  size_t getPriority(const T& task) {
    ld_check(task);
    size_t rv = static_cast<size_t>(task->getPriority());
    ld_check(rv < NumPriorities);
    return rv;
  }
  bool writeIfNotFull(T task) {
    shared_lock<folly::SharedMutex> l(introspection_mutex_);
    bool rv = queues_[getPriority(task)].writeIfNotFull(task);
    if (rv) {
      sem_.post();
    }
    return rv;
  }
  void blockingWrite(T task) {
    shared_lock<folly::SharedMutex> l(introspection_mutex_);
    queues_[getPriority(task)].blockingWrite(task);
    sem_.post();
  }

  void blockingRead(T& out) {
    sem_.wait();
    readQueueGuaranteedNonEmpty(out);
  }

  bool read(T& out) {
    if (!sem_.try_wait()) {
      return false;
    }
    readQueueGuaranteedNonEmpty(out);
    return true;
  }

  void readQueueGuaranteedNonEmpty(T& out) {
    shared_lock<folly::SharedMutex> l(introspection_mutex_);

    // Highest to lowest, yielding if required due to the yield schedule.
    for (int pri = NumPriorities - 1; pri >= 0; --pri) {
      if (queues_[pri].readIfNotEmpty(out)) {
        return;
      }
    }

    // To get here, we probably yielded and skipped one or more priority levels
    // that had an element, or another thread must have added a higher priority
    // task after we examined the queue, and grabbed a lower one before we
    // could. Do a reverse priority scan to find it since the intent of yielding
    // is to allow lower priority items to be serviced.
    //
    // It's also possible that, after we checked a few high priority queues,
    // another thread added a high priority task into one of those queues, which
    // woke up another consumer, which consumed a low priority task.
    //
    // In this loop, becasue we scan in the opposite direction, we can get the
    // opposite problem: new items addded at lower priorities after we scan
    // them, then higher prioirty items removed before we can find them.  That
    // is why we loop indefinitely.

    for (int i = 0;; i++) {
      for (int pri = 0; pri < NumPriorities; ++pri) {
        if (queues_[pri].readIfNotEmpty(out)) {
          return;
        }
      }

      // stats_ can be nullptr in tests.
      if (stats_) {
        if (i == 0) {
          STAT_INCR(stats_, prioritized_queue_cant_find_once);
        } else if (i == 1) {
          STAT_INCR(stats_, prioritized_queue_cant_find_twice);
        } else {
          STAT_INCR(stats_, prioritized_queue_cant_find_thrice_or_more);
        }

        STAT_SET(stats_, prioritized_queue_cant_find_max, i + 1);
      }
    }
  }

  // same as read(), but reads at the specified priority only
  bool readPriority(size_t pri, T& out) {
    if (!sem_.try_wait()) {
      return false;
    }
    shared_lock<folly::SharedMutex> l(introspection_mutex_);

    // using readIfNotEmpty() below instead of read() as we can't afford to
    // not ship a queue entry after decrementing the semaphore
    if (queues_[pri].readIfNotEmpty(out)) {
      return true;
    } else {
      // We have to bump the semaphore back so someone else could pop that
      // element off the queue
      sem_.post();
    }
    return false;
  }

  // reads a batch of tasks, but only within the highest non-empty priority
  folly::small_vector<T, 4> readBatchSinglePriority(size_t max_size,
                                                    size_t max_bytes) {
    folly::small_vector<T, 4> res;
    T item;
    int pri = -1;
    size_t res_bytes = 0;

    while (res.size() < max_size && res_bytes < max_bytes) {
      bool rv;
      if (pri == -1) {
        // first task, reading any priority and setting it
        rv = read(item);
        if (rv) {
          pri = getPriority(item);
        }
      } else {
        // subsequent tasks, fetching one priority only
        rv = readPriority(pri, item);
      }
      if (!rv) {
        break;
      }
      res_bytes += item->getPayloadSize();
      res.push_back(item);
    }

    return res;
  }
  ssize_t size() const {
    shared_lock<folly::SharedMutex> l(introspection_mutex_);
    ssize_t res = 0;
    for (auto& q : queues_) {
      res += q.size();
    }
    return res;
  }
  ssize_t max_capacity() const {
    shared_lock<folly::SharedMutex> l(introspection_mutex_);
    ssize_t res = 0;
    for (auto& q : queues_) {
      res += q.capacity();
    }
    return res;
  }
  // This function introspects contents of the queue and calls cb() on every
  // element in order - from the highest priority to the lowest. It is the only
  // method that locks the mutex in exclusive mode on every call;
  // readQueueGuaranteedNonEmpty() also locks it in rare cases where its
  // having trouble making progress.
  void introspect_contents(std::function<void(T&)> cb) {
    std::unique_lock<folly::SharedMutex> l(introspection_mutex_);
    for (int pri = NumPriorities - 1; pri >= 0; --pri) {
      std::vector<T> queue_contents;
      T out;
      while (queues_[pri].read(out)) {
        cb(out);
        queue_contents.push_back(std::move(out));
      }
      for (T& item : queue_contents) {
        queues_[pri].blockingWrite(std::move(item));
      }
    }
  }

 private:
  // Fixed integer math scale factor for testing ratio of read attempts
  // to yields for each priority class.
  static constexpr int64_t YIELD_SCALE = 1000;
  std::vector<folly::MPMCQueue<T>> queues_;

  Semaphore sem_;

  /**
   * The pointer to stats.
   */
  StatsHolder* stats_;

  // All operations on the queue are usually fully thread-safe without any
  // locking except introspection into the contents of the queue, which has to
  // lock it to ensure that there's both no reordering and no queue overflow
  // (which could happen if an empty-and-refill solution was used instead). This
  // mutex is used counter-intuitively - it's only locked in exclusive mode when
  // reading the queue for introspection, and locked in shared mode when the
  // queue is being normally written to / read from.
  //
  // There's also a race condition on reading, so in rare cases when we can't
  // find an item using a shared lock, we switch to an exclusive lock, which is
  // guaranteed to succeed.
  mutable folly::SharedMutex introspection_mutex_;
};

}} // namespace facebook::logdevice
