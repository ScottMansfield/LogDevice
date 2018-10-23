/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include "logdevice/common/ConfigSource.h"
#include "logdevice/common/Processor.h"
#include "logdevice/common/settings/Settings.h"
#include "logdevice/include/ConfigSubscriptionHandle.h"

namespace facebook { namespace logdevice {

class LegacyPluginPack;
class LogsConfig;
class PluginRegistry;
class UpdateableConfig;

/**
 * @file Config source that gets configs from LogDevice servers. Can accept
 * multiple seed hosts in the config path. The config path should be of the
 * form "server:<host1>,<host2>,<host3>...".
 */

class ServerConfigSource : public ConfigSource {
 public:
  explicit ServerConfigSource(const LogsConfig* alternative_logs_config,
                              std::shared_ptr<LegacyPluginPack> plugin,
                              std::shared_ptr<PluginRegistry> plugin_registry)
      : alternative_logs_config_(alternative_logs_config),
        plugin_(std::move(plugin)),
        plugin_registry_(std::move(plugin_registry)) {}
  ~ServerConfigSource() override {
    // The local processor needs to shutdown its workers first, before anything
    // else gets destroyed
    processor_.reset();
  }

  std::string getName() override {
    return "server";
  }
  std::vector<std::string> getSchemes() override {
    return {"server"};
  }

  Status getConfig(const std::string& path, Output* out) override;

 private:
  std::shared_ptr<Processor> processor_;
  UpdateableSettings<Settings> updateable_settings_;
  std::shared_ptr<UpdateableConfig> config_;
  const LogsConfig* alternative_logs_config_;
  std::shared_ptr<LegacyPluginPack> plugin_;
  std::shared_ptr<PluginRegistry> plugin_registry_;
  ConfigSubscriptionHandle server_config_subscription_;

  void init(const std::string& path, const std::vector<std::string>& hosts);
  bool fetch(const std::string& host);
};

}} // namespace facebook::logdevice
