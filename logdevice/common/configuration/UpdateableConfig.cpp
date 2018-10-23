/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/common/configuration/UpdateableConfig.h"

#include <functional>

#include "logdevice/common/configuration/LocalLogsConfig.h"

namespace facebook { namespace logdevice {

UpdateableConfig::UpdateableConfig(
    std::shared_ptr<UpdateableServerConfig> updateable_server_config,
    std::shared_ptr<UpdateableLogsConfig> updateable_logs_config,
    std::shared_ptr<UpdateableZookeeperConfig> updatable_zookeeper_config)
    : updateable_server_config_(updateable_server_config),
      updateable_logs_config_(updateable_logs_config),
      updatable_zookeeper_config_(updatable_zookeeper_config) {
  if (updateable_logs_config_) {
    logs_config_subscription_ = updateable_logs_config_->subscribeToUpdates(
        std::bind(&UpdateableConfig::notify, this));
  }
  if (updateable_server_config_) {
    server_config_subscription_ = updateable_server_config_->subscribeToUpdates(
        std::bind(&UpdateableConfig::notify, this));
  }
  if (updatable_zookeeper_config) {
    zookeeper_config_subscription_ =
        updatable_zookeeper_config_->subscribeToUpdates(
            std::bind(&UpdateableConfig::notify, this));
  }
}

UpdateableConfig::UpdateableConfig(std::shared_ptr<Configuration> init_config)
    : updateable_server_config_(std::make_shared<UpdateableServerConfig>()),
      updateable_logs_config_(std::make_shared<UpdateableLogsConfig>()),
      updatable_zookeeper_config_(
          std::make_shared<UpdateableZookeeperConfig>()) {
  if (init_config) {
    updateable_server_config_->update(init_config->serverConfig());
    updateable_logs_config_->update(init_config->logsConfig());

    auto& zookeeper_config = init_config->zookeeperConfig();
    if (zookeeper_config != nullptr) {
      updatable_zookeeper_config_->update(zookeeper_config);
    }
  }
  logs_config_subscription_ = updateable_logs_config_->subscribeToUpdates(
      std::bind(&UpdateableConfig::notify, this));
  server_config_subscription_ = updateable_server_config_->subscribeToUpdates(
      std::bind(&UpdateableConfig::notify, this));
  zookeeper_config_subscription_ =
      updatable_zookeeper_config_->subscribeToUpdates(
          std::bind(&UpdateableConfig::notify, this));
}

UpdateableConfig::~UpdateableConfig() = default;

std::shared_ptr<configuration::LocalLogsConfig>
UpdateableConfig::getLocalLogsConfig() const {
  return checked_downcast<std::shared_ptr<configuration::LocalLogsConfig>>(
      updateable_logs_config_->get());
}

std::shared_ptr<UpdateableConfig> UpdateableConfig::createEmpty() {
  auto updateable_config = std::make_shared<UpdateableConfig>();
  auto empty_config = ServerConfig::createEmpty();
  updateable_config->updateableServerConfig()->update(std::move(empty_config));
  updateable_config->updateableLogsConfig()->update(
      std::make_shared<configuration::LocalLogsConfig>());
  updateable_config->updateableZookeeperConfig()->update(
      std::make_shared<configuration::ZookeeperConfig>());
  return updateable_config;
}

}} // namespace facebook::logdevice
