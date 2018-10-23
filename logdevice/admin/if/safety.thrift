/**
 * Copyright (c) 2018-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

include "common.thrift"
include "nodes.thrift"

namespace cpp2 facebook.logdevice.thrift
namespace py3 logdevice.admin
namespace php LogDevice


enum OperationImpact {
  INVALID = 0,
  // This means that rebuilding will not be able to complete given the current
  // status of ShardDataHealth. This assumes that rebuilding may need to run at
  // certain point. This can happen if we have epochs that have lost so many
  // _writable_ shards in its storage-set that it became impossible to amend
  // copysets according to the replication property.
  REBUILDING_STALL = 1,
  // This means that if we perform the operation, we will not be able to
  // generate a nodeset that satisfy the current replication policy for all logs
  // in the log tree.
  WRITE_AVAILABILITY_LOSS = 2,
  // This means that this operation _might_ lead to stalling readers in cases
  // were they need to establish f-majority on some records.
  READ_AVAILABILITY_LOSS = 3,
}

// A data structure that describe the operation impact on a specific epoch in a
// log.
struct ImpactOnEpoch {
  // if log_id == 0 this is the metadata log.
  1: required common.unsigned64 log_id,
  // if log_id == 0, epoch will be zero as well.
  2: required common.unsigned64 epoch,
  // What is the storage set for this epoch (aka. NodeSet)
  3: required common.StorageSet storage_set,
  // What is the replication policy for this particular epoch
  4: required common.ReplicationProperty replication,
  5: required list<OperationImpact> impact,
}

struct CheckImpactRequest {
  // Which shards/nodes we would like to check state change against. Using the
  // ShardID data structure you can refer to individual shards or entire storage
  // or sequencer nodes based on their address or index.
  1: required common.ShardSet shards,
  // This can be unset ONLY if disable_sequencers is set to true. In this case
  // we are only interested in checking for sequencing capacity constraints of
  // the cluster. Alternatively, you can set target_storage_state to READ_WRITE.
  2: optional nodes.ShardStorageState target_storage_state,
  // Do we want to validate if sequencers will be disabled on these nodes as
  // well?
  3: optional bool disable_sequencers,
  // The set of shards that you would like to update their state
  // How much of the location-scope do we want to keep as a safety margin. This
  // assumes that X number of LocationScope can be impacted along with the list
  // of shards/nodes that you supply in the request.
  4: optional common.ReplicationProperty safety_margin,
  // Choose which log-ids to check for safety. Remember that we will always
  // check the metadata and internal logs. This is to ensure that no matter
  // what, operations are safe to these critical logs.
  5: optional list<common.unsigned64> log_ids_to_check,
  // Defaulted to true. In case we found a reason why we think the operation is
  // unsafe, we will not check the rest of the logs.
  6: optional bool abort_on_negative_impact = true,
  // In case the operation is unsafe, how many example ImpactOnEpoch records you
  // want in return?
  7: optional i32 return_sample_size = 50,
}

struct CheckImpactResponse {
  // empty means that no impact, operation is SAFE.
  1: required list<OperationImpact> impact,
  // Only set if there is impact. This indicates whether there will be effect on
  // the metadata logs or the internal state machine logs.
  2: optional bool internal_logs_affected,
  // A sample of the affected epochs by this operation.
  3: optional list<ImpactOnEpoch> logs_affected,
}
