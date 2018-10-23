/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logdevice/admin/Conv.h"

using namespace facebook::logdevice::configuration;
namespace facebook { namespace logdevice {

template <>
thrift::Role toThrift(const NodeRole& role) {
  switch (role) {
    case NodeRole::SEQUENCER:
      return thrift::Role::SEQUENCER;
    case NodeRole::STORAGE:
      return thrift::Role::STORAGE;
  }
  ld_check(false);
  return thrift::Role::STORAGE;
}

template <>
std::vector<thrift::OperationImpact> toThrift(const int& impact_result) {
  std::vector<thrift::OperationImpact> output;
  if (impact_result & Impact::ImpactResult::REBUILDING_STALL) {
    output.push_back(thrift::OperationImpact::REBUILDING_STALL);
  }
  if (impact_result & Impact::ImpactResult::WRITE_AVAILABILITY_LOSS) {
    output.push_back(thrift::OperationImpact::WRITE_AVAILABILITY_LOSS);
  }
  if (impact_result & Impact::ImpactResult::READ_AVAILABILITY_LOSS) {
    output.push_back(thrift::OperationImpact::READ_AVAILABILITY_LOSS);
  }
  return output;
}

template <>
NodeRole toLogDevice(const thrift::Role& role) {
  switch (role) {
    case thrift::Role::SEQUENCER:
      return NodeRole::SEQUENCER;
    case thrift::Role::STORAGE:
      return NodeRole::STORAGE;
  }
  ld_check(false);
  return NodeRole::SEQUENCER;
}

template <>
thrift::ShardStorageState toThrift(const StorageState& storage_state) {
  switch (storage_state) {
    case StorageState::DISABLED:
      return thrift::ShardStorageState::DISABLED;
    case StorageState::READ_ONLY:
      return thrift::ShardStorageState::READ_ONLY;
    case StorageState::READ_WRITE:
      return thrift::ShardStorageState::READ_WRITE;
  }
  ld_check(false);
  return thrift::ShardStorageState::DISABLED;
}

template <>
configuration::StorageState
toLogDevice(const thrift::ShardStorageState& storage_state) {
  switch (storage_state) {
    case thrift::ShardStorageState::DISABLED:
      return StorageState::DISABLED;
    case thrift::ShardStorageState::READ_ONLY:
      return StorageState::READ_ONLY;
    case thrift::ShardStorageState::READ_WRITE:
      return StorageState::READ_WRITE;
  }
  ld_check(false);
  return StorageState::DISABLED;
}

template <>
NodeLocationScope toLogDevice(const thrift::LocationScope& location_scope) {
  switch (location_scope) {
#define NODE_LOCATION_SCOPE(name)   \
  case thrift::LocationScope::name: \
    return NodeLocationScope::name;
#include "logdevice/include/node_location_scopes.inc"
  }
  ld_check(false);
  return NodeLocationScope::INVALID;
}

template <>
thrift::LocationScope toThrift(const NodeLocationScope& location_scope) {
  switch (location_scope) {
    case NodeLocationScope::INVALID:
      // We don't have INVALID in thrift because we don't need it.
      return thrift::LocationScope::ROOT;
#define NODE_LOCATION_SCOPE(name) \
  case NodeLocationScope::name:   \
    return thrift::LocationScope::name;
#include "logdevice/include/node_location_scopes.inc"
  }
  ld_check(false);
  return thrift::LocationScope::ROOT;
}

template <>
ReplicationProperty
toLogDevice(const thrift::ReplicationProperty& replication) {
  std::vector<ReplicationProperty::ScopeReplication> vec;
  for (const auto& it : replication) {
    vec.push_back(
        std::make_pair(toLogDevice<NodeLocationScope>(it.first), it.second));
  }
  return ReplicationProperty(std::move(vec));
}

template <>
thrift::ShardID toThrift(const ShardID& shard) {
  thrift::ShardID output;
  output.set_shard_index(shard.shard());
  // We do not set the address of the node in the output. This can be useful
  // in the future if we needed to always locate the nodes with their address
  // instead of the index. However, it's not necessary right now.
  // TODO: Also return node address information.
  thrift::NodeID node_identifier;
  node_identifier.set_node_index(shard.node());
  output.set_node(std::move(node_identifier));
  return output;
}

template <>
thrift::StorageSet toThrift(const StorageSet& storage_set) {
  thrift::StorageSet output;
  for (const auto& shard : storage_set) {
    output.push_back(toThrift<thrift::ShardID>(shard));
  }
  return output;
}

template <>
thrift::ReplicationProperty toThrift(const ReplicationProperty& replication) {
  thrift::ReplicationProperty output;
  for (const auto& scope_replication :
       replication.getDistinctReplicationFactors()) {
    output.insert(
        std::make_pair(toThrift<thrift::LocationScope>(scope_replication.first),
                       scope_replication.second));
  }
  return output;
}

template <>
thrift::ImpactOnEpoch toThrift(const Impact::ImpactOnEpoch& epoch) {
  thrift::ImpactOnEpoch output;
  output.set_epoch(static_cast<int64_t>(epoch.epoch.val_));
  output.set_log_id(static_cast<int64_t>(epoch.log_id.val_));
  output.set_storage_set(toThrift<thrift::StorageSet>(epoch.storage_set));
  output.set_replication(
      toThrift<thrift::ReplicationProperty>(epoch.replication));
  output.set_impact(
      toThrift<std::vector<thrift::OperationImpact>>(epoch.impact_result));
  return output;
}
}} // namespace facebook::logdevice
