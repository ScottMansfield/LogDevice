/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <chrono>
#include <memory>

#include "SafetyAPI.h"
#include "logdevice/common/EpochMetaData.h"
#include "logdevice/common/FailureDomainNodeSet.h"
#include "logdevice/common/NodeSetFinder.h"
#include "logdevice/common/Request.h"
#include "logdevice/common/ShardAuthoritativeStatusMap.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/configuration/Configuration.h"
#include "logdevice/include/Err.h"
#include "logdevice/include/Record.h"
#include "logdevice/include/types.h"

namespace facebook { namespace logdevice {

// read the metadata log record by record, and determine
// is it safe to drain specified shards

class ClientImpl;

class CheckMetaDataLogRequest : public Request {
 public:
  // callback function called when the operation is complete
  // The epoch_t and StorageSet parameters are valid only if Status != E::OK
  using Callback = folly::Function<void(
      Status,
      int, // impact result bit set
      logid_t,
      epoch_t,    // Which epoch in logid_t has failed the safety check
      StorageSet, // The storage set that caused the failure (if st != E::OK)
      ReplicationProperty // The replication property for the offending epoch
      )>;

  /**
   * create a CheckMetaDataLogRequest. CheckMetaDataLogRequest verifies that
   * it is safe to set the storage state to 'target_storage_state' for
   op_shards.
   * If it is data log (check_metadata=false) it reads corresponding metadata
   * log records by record to do so.
   * If check_metadata=true, it ignores log_id and verifies we have enough nodes
   * for metadata after operations
   *
   *  @param log_id          data log id to perform check. Ignored
   *                         if check_metadata=true
   *  @param timeout         Timeout to use
   *  @param shard_status    shard authoritative status map
   *  @param op_shards       check those shards if it is safe to perform
   *                         operations on them
   *  @param target_storage_state
   *                         The configuration::StorageState that we want
   *                         to set
   *  @safety_margin         safety margin (number of domains in each scope)
                             which we could afford to lose after operations
   *  @param abort_on_error  abort on the first problem detected
   *  @param check_metadata  check metadata logs instead
   *  @param callback        callback to provide result of check
   */
  CheckMetaDataLogRequest(logid_t log_id,
                          std::chrono::milliseconds timeout,
                          ShardAuthoritativeStatusMap shard_status,
                          ShardSet op_shards,
                          configuration::StorageState target_storage_state,
                          SafetyMargin safety_margin,
                          bool check_meta_nodeset,
                          WorkerType worker_type_,
                          Callback callback);

  ~CheckMetaDataLogRequest() override;

  WorkerType getWorkerTypeAffinity() override;

  Request::Execution execute() override;
  void complete(Status st,
                int = Impact::ImpactResult::INVALID, // impact result bit set
                epoch_t error_epoch = EPOCH_INVALID,
                StorageSet storage_set = {},
                ReplicationProperty replication = ReplicationProperty());

  /*
   * Instructs this utility to not assume the server is recent enough to be able
   * to serve EpochMetaData from the sequencer.
   */
  void readEpochMetaDataFromSequencer() {
    read_epoch_metadata_from_sequencer_ = true;
  }

 private:
  // Use NodeSetFinder to find historical metadata.
  void fetchHistoricalMetadata();

  // callback for delivering a metadata log record
  // returns false if an error was found and complete() was called.
  bool onEpochMetaData(EpochMetaData metadata);

  /**
   * Checks whether write availability in this storage set would be lost if we
   * were to perform the drain.
   * @param storage_set   storage set to verify
   * @param replication   replication property of storage set
   * @return true if the storage set will not be affected by the drain, false
   * otherwise
   */
  bool checkWriteAvailability(const StorageSet& storage_set,
                              const ReplicationProperty& replication,
                              NodeLocationScope* fail_scope) const;

  /**
   * Checks whether read availability in this storage set would be lost if we
   * were to disable reads.
   * @param storage_set   storage set to verify
   * @param replication   replication property of storage set
   * @return true if the storage set will not be affected by the operation,
   * false otherwise
   */
  bool checkReadAvailability(const StorageSet& storage_set,
                             const ReplicationProperty& replication) const;

  bool isAlive(node_index_t index) const;

  /**
   * Verifies that it is possible to perform operations_
   * without losing read/write availability of metadata nodes
   */
  void checkMetadataNodeset();

  /**
   * Create modified ReplicationProperty which takes into account Safety Margin.
   * For write check (canDrain) we should add it, for read check (isFmajority)
   * we should subtract
   **/
  ReplicationProperty
  extendReplicationWithSafetyMargin(const ReplicationProperty& replication_base,
                                    bool add) const;

  std::tuple<bool, bool, NodeLocationScope>
  checkReadWriteAvailablity(const StorageSet& storage_set,
                            const ReplicationProperty& replication_property);

  const logid_t log_id_;
  std::chrono::milliseconds timeout_;
  // TODO(T28386689): remove once all production tiers are on 2.35.
  bool read_epoch_metadata_from_sequencer_ = false;

  std::unique_ptr<NodeSetFinder> nodeset_finder_;

  // worker index to run the request
  worker_id_t current_worker_{-1};

  ShardAuthoritativeStatusMap shard_status_;
  ShardSet op_shards_;
  configuration::StorageState target_storage_state_;

  // safety margin for each replication scope
  // we consider operations safe, only if after draining/stopping
  // we would still have extra (safety margin) domains to loose in each scope
  // lifetime managed by SafetyChecker
  SafetyMargin safety_margin_;

  const bool check_metadata_nodeset_;
  WorkerType worker_type_;
  // callback function provided by user of the class. Called when the state
  // machine completes.
  Callback callback_;
};
}} // namespace facebook::logdevice
