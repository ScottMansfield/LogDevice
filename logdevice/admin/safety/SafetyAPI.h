/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <map>

#include "logdevice/common/ShardID.h"
#include "logdevice/common/configuration/ReplicationProperty.h"
#include "logdevice/common/types_internal.h"
#include "logdevice/include/Err.h"
#include "logdevice/include/NodeLocationScope.h"
#include "logdevice/include/types.h"

namespace facebook { namespace logdevice {

using SafetyMargin = std::map<NodeLocationScope, int>;

struct Impact {
  /**
   * A data structure that holds the operation impact on a specific epoch in a
   * log.
   */
  struct ImpactOnEpoch {
    logid_t log_id = LOGID_INVALID;
    epoch_t epoch = EPOCH_INVALID;
    StorageSet storage_set;
    ReplicationProperty replication;
    int impact_result = ImpactResult::INVALID;
    ImpactOnEpoch(logid_t log_id,
                  epoch_t epoch,
                  StorageSet storage_set,
                  ReplicationProperty replication,
                  int impact_result)
        : log_id(log_id),
          epoch(epoch),
          storage_set(storage_set),
          replication(std::move(replication)),
          impact_result(impact_result) {}
  };

  enum ImpactResult {
    NONE = 0,
    // operation could lead to rebuilding stall, as full rebuilding
    // of all logs a is not possible due to historical nodesets
    REBUILDING_STALL = (1u << 1),
    // operation could lead to loss of write availability.
    WRITE_AVAILABILITY_LOSS = (1u << 2),
    // operation could lead to loss of read availability,
    // as there is no f-majority for certain logs
    READ_AVAILABILITY_LOSS = (1u << 3),
    // Impact Could not be established due to an error.
    INVALID = (1u << 30),
  };

  // What was the status of the operation. E::OK means that it's safe to trust
  // the result of the check impact request. Any other value means that some
  // error has happened. In such case the ImpactResult will be set to INVALID
  Status status;
  // bit set of ImpactResult
  int32_t result;

  // Set of data logs affected.
  std::vector<ImpactOnEpoch> logs_affected;

  // Whether metadata logs are also affected (ie the operations have impact on
  // the metadata nodeset or the internal logs).
  bool internal_logs_affected;

  Impact(Status status,
         int result,
         std::vector<ImpactOnEpoch> logs_affected = {},
         bool internal_logs_affected = false);

  // A helper constructor that must only be used if status != E::OK
  explicit Impact(Status status);
  // Empty constructor sets everything as if the operation is safe.
  Impact();

  std::string toString() const;
  static std::string toStringImpactResult(int);
};

/**
 * @param descriptor A descriptor describing one safety margin or a set of .
                     safety marings. Safety marging is simular to replication
                     property - it is list of <domain>:<number> pairs.
 *                   For instance, \"rack:1\",\"node:2\".
 * @param out        Populated set of NodeLocationScope, int  pairs.
 *
 * @return           0 on success, or -1 on error
 */
int parseSafetyMargin(const std::vector<std::string>& descriptors,
                      SafetyMargin& out);

// Converts a ReplicationProperty object into SafetyMargin
// TODO: We should replace SafetyMargin type with ReplicationProperty
SafetyMargin
safetyMarginFromReplication(const ReplicationProperty& replication);

/**
 * @param descriptors A list of descriptors, @see parseSafetyMargin.
 * @param out        Populated set of NodeLocationScope, int  pairs.
 *
 * @return           0 on success, or -1 on error
 *
 */
int parseSafetyMargin(const std::string& descriptor, SafetyMargin& out);
}} // namespace facebook::logdevice
