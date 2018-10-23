/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <atomic>
#include <memory>

#include "logdevice/common/EventLoopHandle.h"
#include "logdevice/common/PermissionChecker.h"
#include "logdevice/common/PrincipalParser.h"
#include "logdevice/common/admin/AdminServer.h"
#include "logdevice/common/configuration/ServerConfig.h"
#include "logdevice/common/configuration/UpdateableConfig.h"
#include "logdevice/common/settings/GossipSettings.h"
#include "logdevice/common/settings/RebuildingSettings.h"
#include "logdevice/common/stats/Stats.h"
#include "logdevice/include/ConfigSubscriptionHandle.h"
#include "logdevice/server/CommandListener.h"
#include "logdevice/server/ConnectionListener.h"
#include "logdevice/server/LocalLogFile.h"
#include "logdevice/server/ServerSettings.h"
#include "logdevice/server/UnreleasedRecordDetector.h"
#include "logdevice/server/locallogstore/LocalLogStoreSettings.h"
#include "logdevice/server/locallogstore/RocksDBSettings.h"

namespace facebook { namespace logdevice {

class LogStoreMonitor;
class MyNodeID;
class PluginRegistry;
class Processor;
class RebuildingCoordinator;
class RebuildingSupervisor;
class SequencerPlacement;
class ServerProcessor;
class SettingsUpdater;
class ShardedRocksDBLocalLogStore;
class ShardedStorageThreadPool;
class TraceLogger;
class UnreleasedRecordDetector;

/**
 * Command line options and configuration needed to run a server.
 * Can be shared between servers.
 */
class ServerParameters {
 public:
  // If something goes wrong prints the error and throws ConstructorFailed
  // without setting err.
  explicit ServerParameters(
      std::shared_ptr<SettingsUpdater> settings_updater,
      UpdateableSettings<ServerSettings> server_settings,
      UpdateableSettings<RebuildingSettings> rebuilding_settings,
      UpdateableSettings<LocalLogStoreSettings> locallogstore_settings,
      UpdateableSettings<GossipSettings> gossip_settings,
      UpdateableSettings<Settings> processor_settings,
      UpdateableSettings<RocksDBSettings> rocksdb_settings,
      std::shared_ptr<PluginRegistry> plugin_registry);
  ~ServerParameters();

  ServerParameters(const ServerParameters& rhs) = delete;
  ServerParameters& operator=(const ServerParameters& rhs) = delete;

  // Not mutually exclusive.
  bool isReadableStorageNode() const;
  bool isSequencingEnabled() const;
  size_t getNumDBShards() const;

  bool isFastShutdownEnabled() const;
  void setFastShutdownEnabled(bool enabled);

  std::shared_ptr<UpdateableConfig> getUpdateableConfig();
  std::shared_ptr<TraceLogger> getTraceLogger();
  const std::shared_ptr<LocalLogFile>& getAuditLog();
  StatsHolder* getStats();
  std::shared_ptr<PluginRegistry> getPluginRegistry() const {
    return plugin_registry_;
  }

  std::shared_ptr<SettingsUpdater> getSettingsUpdater();
  const UpdateableSettings<RebuildingSettings>& getRebuildingSettings() const {
    return rebuilding_settings_;
  }

  const UpdateableSettings<ServerSettings>& getServerSettings() const {
    return server_settings_;
  }

  const UpdateableSettings<LocalLogStoreSettings>&
  getLocalLogStoreSettings() const {
    return locallogstore_settings_;
  }

  const UpdateableSettings<GossipSettings>& getGossipSettings() const {
    return gossip_settings_;
  }
  const UpdateableSettings<Settings>& getProcessorSettings() const {
    return processor_settings_;
  }

  const UpdateableSettings<RocksDBSettings> getRocksDBSettings() const {
    return rocksdb_settings_;
  }

 private:
  std::shared_ptr<PluginRegistry> plugin_registry_;
  StatsHolder server_stats_;
  std::shared_ptr<SettingsUpdater> settings_updater_;
  UpdateableSettings<ServerSettings> server_settings_;
  UpdateableSettings<RebuildingSettings> rebuilding_settings_;
  UpdateableSettings<LocalLogStoreSettings> locallogstore_settings_;
  UpdateableSettings<GossipSettings> gossip_settings_;
  UpdateableSettings<Settings> processor_settings_;
  UpdateableSettings<RocksDBSettings> rocksdb_settings_;

  bool storage_node_;
  size_t num_db_shards_{0}; // Set to zero if !storage_node_.
  bool run_sequencers_;
  std::atomic_bool fast_shutdown_enabled_{false};

  std::unique_ptr<MyNodeID> my_node_id_extractor_;
  std::shared_ptr<UpdateableConfig> updateable_config_;
  std::shared_ptr<TraceLogger> trace_logger_;
  std::shared_ptr<LocalLogFile> audit_log_;

  // Assigned when config is loaded.
  folly::Optional<node_index_t> my_node_index_;

  // Handle for the subscription to config updates, used to unsubscribe
  std::list<ConfigSubscriptionHandle> server_config_subscriptions_;
  std::list<ConfigSubscriptionHandle> logs_config_subscriptions_;
  std::list<UpdateableServerConfig::HookHandle> server_config_hook_handles_;

  // Sets Settings::max_{accepted,client}_connections based on the fd limit,
  // number of reserved fds, as well as the number of nodes in the cluster.
  bool setConnectionLimits();

  bool validateNodes(ServerConfig& config);
  bool updateMyNodeId(ServerConfig& config);
  bool updateConfigSettings(ServerConfig& config);
};

/**
 * Conains everything necessary to run a server.
 */
class Server {
 public:
  // The constructor calls _exit(EXIT_FAILURE) if any subsystem fails to
  // initialize.
  Server(ServerParameters* params, std::function<void()> stop_handler);

  // Shuts down the server.
  // If graceful shutdown times out, does _exit(EXIT_FAILURE);
  ~Server();

  Server(const Server& rhs) = delete;
  Server& operator=(const Server& rhs) = delete;

  // Kick off command and connection listeners' event loops.
  bool startListening();

  ServerParameters* getParameters() {
    return params_;
  }
  std::chrono::system_clock::time_point getStartTime() {
    return start_time_;
  }
  ServerProcessor* getServerProcessor() const {
    return processor_.get();
  }
  Processor* getProcessor() const;
  ShardedRocksDBLocalLogStore* getShardedLocalLogStore() {
    return sharded_store_.get();
  }

  // Calls stop_handler_, which shuts the server down gracefully soon after.
  void requestStop();

  // Unlike ~Server, doesn't have a time limit.
  void gracefulShutdown();

  RebuildingCoordinator* getRebuildingCoordinator();

  SettingsUpdater& getSettings() {
    return *settings_updater_;
  }

  const UpdateableSettings<ServerSettings>& getServerSettings() const {
    return server_settings_;
  }

  RebuildingSupervisor* getRebuildingSupervisor() {
    return rebuilding_supervisor_.get();
  }

  // For tests, to help simulate various forms of network partition.
  void acceptNewConnections(bool accept) {
    checked_downcast<Listener*>(connection_listener_handle_->get())
        ->acceptNewConnections(accept);
    checked_downcast<Listener*>(ssl_connection_listener_handle_->get())
        ->acceptNewConnections(accept);
  }

  void rotateLocalLogs();

 private:
  ServerParameters* params_;
  std::function<void()> stop_handler_;

  std::chrono::system_clock::time_point start_time_;

  UpdateableSettings<ServerSettings> server_settings_;
  std::shared_ptr<UpdateableConfig> updateable_config_;
  std::shared_ptr<ServerConfig> server_config_;
  std::shared_ptr<SettingsUpdater> settings_updater_;

  // initListeners()
  std::unique_ptr<EventLoopHandle> connection_listener_handle_;
  std::unique_ptr<EventLoopHandle> ssl_connection_listener_handle_;
  std::unique_ptr<EventLoopHandle> command_listener_handle_;
  std::unique_ptr<EventLoopHandle> gossip_listener_handle_;
  std::unique_ptr<AdminServer> admin_server_handle_;

  // initStore()
  std::unique_ptr<ShardedRocksDBLocalLogStore> sharded_store_;
  std::unique_ptr<ShardedStorageThreadPool> sharded_storage_thread_pool_;

  // initProcessor()
  std::shared_ptr<ServerProcessor> processor_;

  // initLogStoreMonitor()
  std::unique_ptr<LogStoreMonitor> logstore_monitor_;

  // initSequencerPlacement()
  UpdateableSharedPtr<SequencerPlacement> sequencer_placement_;

  // initRebuildingCoordinator()
  // only populated if this node is a storage node.
  std::unique_ptr<RebuildingCoordinator> rebuilding_coordinator_;

  std::unique_ptr<RebuildingSupervisor> rebuilding_supervisor_;

  // initUnreleasedRecordDetector()
  // only populated if this node is a storage node.
  std::shared_ptr<UnreleasedRecordDetector> unreleased_record_detector_;

  // gracefulShutdown() was called
  std::atomic<bool> is_shut_down_{false};

  // initSettingsSubscriber()
  UpdateableSettings<Settings>::SubscriptionHandle
      settings_subscription_handle_;

  // These methods should be called in this order.
  // In case of error, log it and return false.
  bool initListeners();
  bool initStore();
  bool initProcessor();
  bool repopulateRecordCaches();
  bool initSequencers();
  bool initLogStoreMonitor();
  bool initFailureDetector();
  bool initSequencerPlacement();
  bool initRebuildingCoordinator();
  bool initUnreleasedRecordDetector();
  bool initLogsConfigManager();
  bool initSettingsSubscriber();

  // Calls gracefulShutdown in separate thread and does _exit(EXIT_FAILURE)
  // if it takes longer than server_settings_->shutdown_timeout ms.
  void shutdownWithTimeout();

  bool startCommandListener(std::unique_ptr<EventLoopHandle>& handle);
  bool startConnectionListener(std::unique_ptr<EventLoopHandle>& handle);

  void updateStatsSettings();
};

}} // namespace facebook::logdevice
