/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <folly/Function.h>

#include "logdevice/common/configuration/nodes/NodesConfiguration.h"
#include "logdevice/include/Err.h"

namespace facebook { namespace logdevice { namespace configuration {

class NodesConfigurationAPI {
 public:
  using CompletionCb =
      foly::Function<void(Status,
                          std::shared_ptr<const nodes::NodesConfiguration>)>;

  virtual int update(nodes::NodesConfiguration::Update update,
                     CompletionCb callback) = 0;

  // unconditionally overwrite the configuration with the provided config.
  // used in emergency
  virtual int
  overwrite(std::shared_ptr<const nodes::NodesConfiguration> configuration,
            CompletionCb callback) = 0;

  virtual ~NodesConfigurationAPI() {}
};

}}} // namespace facebook::logdevice::configuration
