/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <memory>
#include <vector>

#include <folly/small_vector.h>

#include "logdevice/common/Semaphore.h"
#include "logdevice/common/SimpleEnumMap.h"
#include "logdevice/common/settings/Settings.h"
#include "logdevice/server/storage_tasks/PrioritizedQueue.h"
#include "logdevice/server/storage_tasks/StorageTask.h"

namespace facebook { namespace logdevice {

/**
 * @file A pool of threads that handle I/O from/to the local log store on
 *       storage nodes.  When worker threads need I/O done, they put
 *       StorageTasks onto the thread pool's queue.
 */

class LocalLogStore;
class ServerProcessor;
class StatsHolder;
class ExecStorageThread;
class SyncingStorageThread;
class TraceLogger;
class WriteStorageTask;

class StorageThreadPool {
 public:
  using WriteTaskQueue =
      PrioritizedQueue<WriteStorageTask*,
                       (size_t)StorageTask::Priority::NUM_PRIORITIES>;
  using TaskQueue =
      PrioritizedQueue<StorageTask*,
                       (size_t)StorageTask::Priority::NUM_PRIORITIES>;
  struct TaskQueueParams {
    int nthreads = 0;
  };
  using Params =
      std::array<TaskQueueParams, (size_t)StorageTaskThreadType::MAX>;

  /**
   * Creates the pool and starts all threads.  Does not claim ownership of the
   * local log store.
   *
   * @throws ConstructorFailed on failure
   */
  StorageThreadPool(shard_index_t shard_idx,
                    const Params& params,
                    UpdateableSettings<Settings> settings,
                    LocalLogStore* local_log_store,
                    size_t task_queue_size,
                    StatsHolder* stats = nullptr,
                    const std::shared_ptr<TraceLogger> trace_logger = nullptr);

  ~StorageThreadPool();

  LocalLogStore& getLocalLogStore() {
    return *local_log_store_;
  }

  const UpdateableSettings<Settings> getSettings() {
    return settings_;
  }

  void setProcessor(ServerProcessor* processor) {
    processor_ = processor;
  }

  ServerProcessor& getProcessor() {
    ld_check(processor_ != nullptr);
    return *processor_;
  }

  std::shared_ptr<TraceLogger> getTraceLogger() const {
    return trace_logger_;
  }

  StatsHolder* stats() {
    return stats_;
  }

  int getShardIdx() {
    return shard_idx_;
  }

  // If true, storage tasks of type FAST_STALLABLE should stall writes
  bool writeStallingEnabled() const {
    return nthreads_fast_stallable_ > 0;
  }

  /**
   * Attempts to put a task onto the queue.  If it succeeds (there is room on
   * the queue), claims ownership of the task.
   *
   * @return On success, returns 0.  On failure, returns -1 and sets err to
   *   SHUTDOWN   if server is shutting down
   *   INTERNAL   if adding task to taskQueue_ failed
   */
  int tryPutTask(std::unique_ptr<StorageTask>&& task);

  /**
   * Attempts to put a write task onto the write queue
   *
   * @return On success, returns 0.  On failure, returns -1 and sets err to
   *   SHUTDOWN   if server is shutting down
   *   INTERNAL   if adding task to writeQueue_ failed
   */
  int tryPutWrite(std::unique_ptr<WriteStorageTask>&& task);

  /**
   * Puts a task onto the queue, blocking until there is room for it.  Claims
   * ownership of the task.
   */
  void blockingPutTask(std::unique_ptr<StorageTask>&& task);

  /**
   * Gets a task from the queue, blocking if there are none.  Used by storage
   * threads to get work to do.
   */
  std::unique_ptr<StorageTask> blockingGetTask(StorageTask::ThreadType type);

  /**
   * Tries to get a batch of WriteStorageTasks from the write queue.
   * @return nullptr if write queue was empty
   */
  folly::small_vector<std::unique_ptr<WriteStorageTask>, 4>
  tryGetWriteBatch(StorageTask::ThreadType thread_type,
                   size_t max_count,
                   size_t max_bytes);

  /**
   * Enqueue the task for syncing to nonvolatile storage.  This is called
   * after the local log store has accepted a write but has not necessarily
   * yet synced it to storage.  After the sync completes, the task will be
   * passed back to the worker.
   */
  void enqueueForSync(std::unique_ptr<StorageTask> task);

  /**
   * Called by a worker thread to suggest that all tasks currently in the
   * shared queue be dropped because the system is overloaded.
   */
  void dropTaskQueue(StorageTask::ThreadType type);

  /**
   * Initiates shutdown: instructs threads to finish processing queued tasks.
   *
   * @param persist_record_caches  indicating whether or not to persist record
   *                               caches
   */
  void shutDown(bool persist_record_caches = false);

  /**
   * Second phase of shutdown: waits for threads to finish and destroys them.
   */
  void join();

  /**
   * Fetches debug info on all pending storage tasks into the table provided
   */
  void getStorageTaskDebugInfo(InfoStorageTasksTable& table);

 private:
  UpdateableSettings<Settings> settings_;
  // Number of storage threads of each type.
  const int nthreads_slow_;
  const int nthreads_fast_stallable_;
  const int nthreads_fast_time_sensitive_;
  const int nthreads_metadata_;

  std::vector<std::unique_ptr<ExecStorageThread>> exec_threads_;

  std::unique_ptr<SyncingStorageThread> syncing_thread_;

  // Pointer to local log store.  Not owned by this.
  LocalLogStore* local_log_store_;

  // Pointer to Processor for sending back replies to worker threads
  // (unowned)
  ServerProcessor* processor_;

  const std::shared_ptr<TraceLogger> trace_logger_;

  struct PerTypeTaskQueue {
    PerTypeTaskQueue(size_t size, StatsHolder* stats)
        : queue(size, stats), write_queue(size, stats), tasks_to_drop(0) {}

    // Task queue. Other threads write into it and our threads read from it.
    TaskQueue queue;
    // Separate queue for write batching
    WriteTaskQueue write_queue;
    // How many tasks should be dropped?
    std::atomic<int64_t> tasks_to_drop;
  };

  // If set, *Put{Task,Write} methods will immediately return (with err set
  // to E::SHUTDOWN). Set by shutDown().
  std::atomic<bool> shutting_down_{false};

  StatsHolder* stats_;

  shard_index_t shard_idx_;

  // Separate queue for each of the two types of storage threads.
  SimpleEnumMap<StorageTask::ThreadType, PerTypeTaskQueue> taskQueues_;

  /**
   * Called when tasksToDrop_ was observed to be more than 0, suggesting that
   * a task should be dropped.
   *
   * @return true if the task was actually dropped, false if not (another
   *         thread beat us to it)
   */
  bool tryDropOneTask(std::unique_ptr<StorageTask>& task);

  /**
   * Called only by the constructor.
   */
  std::array<size_t, (size_t)StorageTask::ThreadType::MAX>
  computeActualQueueSizes(size_t task_queue_size) const;

  StorageTask::ThreadType getThreadType(const StorageTask& task) const;
  StorageTask::ThreadType getThreadType(StorageTask::ThreadType type) const;
};
}} // namespace facebook::logdevice
