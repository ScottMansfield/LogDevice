/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include "logdevice/common/configuration/nodes/NodeAttributesConfig.h"
#include "logdevice/common/configuration/nodes/PerRoleConfig.h"
#include "logdevice/common/membership/SequencerMembership.h"

namespace facebook { namespace logdevice { namespace configuration {
namespace nodes {

struct SequencerNodeAttribute {
  bool isValid() const {
    return true;
  }
};

using SequencerAttributeConfig =
    NodeAttributesConfig<SequencerNodeAttribute, /*Mutable=*/true>;

using SequencerConfig = PerRoleConfig<NodeRole::SEQUENCER,
                                      membership::SequencerMembership,
                                      SequencerAttributeConfig>;

}}}} // namespace facebook::logdevice::configuration::nodes
