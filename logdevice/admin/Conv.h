/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "logdevice/admin/if/gen-cpp2/admin_types.h"
#include "logdevice/admin/safety/SafetyAPI.h"
#include "logdevice/common/configuration/Node.h"
#include "logdevice/include/NodeLocationScope.h"

namespace facebook { namespace logdevice {

inline uint64_t to_unsigned(thrift::unsigned64 value) {
  return static_cast<uint64_t>(value);
}

// Templated thrift <-> LogDevice type converter. This interface can be used if
// there is 1:1 mapping between the types.
template <typename ThriftType, typename LDType>
ThriftType toThrift(const LDType& input);

/** Specializations **/
template <>
thrift::ShardStorageState toThrift(const configuration::StorageState& input);

template <>
thrift::Role toThrift(const configuration::NodeRole& role);

template <>
std::vector<thrift::OperationImpact>
toThrift(const Impact::ImpactResult& impact);

template <>
thrift::ShardID toThrift(const ShardID& shard);

template <>
thrift::StorageSet toThrift(const StorageSet& storage_set);

template <>
thrift::LocationScope toThrift(const NodeLocationScope& input);

template <>
thrift::ReplicationProperty toThrift(const ReplicationProperty& replication);

template <>
thrift::ImpactOnEpoch toThrift(const Impact::ImpactOnEpoch& epoch);

template <typename LDType, typename ThriftType>
LDType toLogDevice(const ThriftType& input);

/** Specializations **/
template <>
configuration::NodeRole toLogDevice(const thrift::Role& role);

template <>
configuration::StorageState toLogDevice(const thrift::ShardStorageState& input);

template <>
NodeLocationScope toLogDevice(const thrift::LocationScope& input);

template <>
ReplicationProperty toLogDevice(const thrift::ReplicationProperty& input);
}} // namespace facebook::logdevice
