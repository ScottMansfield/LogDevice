/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "logdevice/admin/safety/SafetyChecker.h"
#include "logdevice/common/configuration/Configuration.h"
#include "logdevice/lib/ClientImpl.h"
#include "logdevice/lib/ops/EventLogUtils.h"
#include "logdevice/test/utils/IntegrationTestBase.h"
#include "logdevice/test/utils/IntegrationTestUtils.h"

using namespace facebook::logdevice;

// There are at least three approaches to test drains / modify node sets
// a) varying size of the cluster and rely on SelectAllNodeSetSelector
// b) use TestNodeSetSelector
// c) use NodeSetTest.CHANGE_NODESET

// (a) is used as direct modification of nodesets from outside of the cluster
// potentially may be disabled in future due to AutoLogProvisioning

const logid_t LOG_ID{1};
const logid_t LOG_ID2{2};

class SafetyAPIIntegrationTest : public IntegrationTestBase {
 protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    dbg::currentLevel = dbg::Level::DEBUG;
  }
};

void write_test_records(std::shared_ptr<Client> client,
                        logid_t logid,
                        size_t num_records) {
  static size_t counter = 0;
  for (size_t i = 0; i < num_records; ++i) {
    std::string data("data" + std::to_string(++counter));
    lsn_t lsn = client->appendSync(logid, Payload(data.data(), data.size()));
    ASSERT_NE(LSN_INVALID, lsn)
        << "Append failed (E::" << error_name(err) << ")";
  }
}

Configuration::Log createLog() {
  Configuration::Log log;
  log.singleWriter = false;
  log.replicationFactor = 3;
  log.extraCopies = 0;
  log.syncedCopies = 0;
  return log;
}

TEST_F(SafetyAPIIntegrationTest, DrainWithExpand) {
  const size_t num_nodes = 3;
  const size_t num_shards = 2;

  Configuration::Nodes nodes;

  for (int i = 0; i < num_nodes; ++i) {
    nodes[i].generation = 1;
    nodes[i].addSequencerRole();
    nodes[i].addStorageRole(num_shards);
  }

  Configuration::Log logcfg;
  logcfg.rangeName = "test_range";
  logcfg.replicationFactor = 2;

  auto meta_configs =
      createMetaDataLogsConfig({0, 2}, 2, NodeLocationScope::NODE);

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNumLogs(1)
                     .setNodes(nodes)
                     // switches on gossip
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .setMetaDataLogsConfig(meta_configs)
                     .setLogConfig(logcfg)
                     .create(num_nodes);

  for (const auto& it : cluster->getNodes()) {
    node_index_t idx = it.first;
    cluster->getNode(idx).waitUntilAvailable();
  }

  std::shared_ptr<Client> client = cluster->createClient();
  ClientImpl* client_impl = dynamic_cast<ClientImpl*>(client.get());

  write_test_records(client, LOG_ID, 10);

  ld_info("Waiting for metadata log writes to complete");
  cluster->waitForMetaDataLogWrites();

  ShardAuthoritativeStatusMap shard_status{LSN_INVALID};
  int rv = cluster->getShardAuthoritativeStatusMap(shard_status);
  ASSERT_EQ(0, rv);

  SafetyChecker safety_checker(
      &client_impl->getProcessor(), 100, true, client_impl->getTimeout());
  ShardSet shards;

  for (int i = 0; i < num_nodes; ++i) {
    for (int s = 0; s < num_shards; ++s) {
      shards.insert(ShardID(i, s));
    }
  }

  // it is unsafe to drain all shards
  Impact impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::READ_ONLY);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::REBUILDING_STALL |
                Impact::ImpactResult::WRITE_AVAILABILITY_LOSS,
            impact.result);

  ASSERT_TRUE(impact.internal_logs_affected);

  // we have replication factor 2, NodeSet includes all nodes
  // it is safe to drain 1 node
  shards.clear();
  for (int i = 0; i < num_shards; ++i) {
    shards.insert(ShardID(1, i));
  }

  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::READ_ONLY);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::NONE, impact.result);

  // draining 2 nodes is unsafe as we will would have not enough nodes
  // to replicate
  for (int i = 0; i < num_shards; ++i) {
    shards.insert(ShardID(2, i));
  }

  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::READ_ONLY);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::REBUILDING_STALL |
                Impact::ImpactResult::WRITE_AVAILABILITY_LOSS,
            impact.result);
  ASSERT_TRUE(impact.internal_logs_affected);

  // double cluster size
  cluster->expand(num_nodes);
  for (const auto& it : cluster->getNodes()) {
    node_index_t idx = it.first;
    cluster->getNode(idx).waitUntilAvailable();
  }

  write_test_records(client, LOG_ID, 10);

  shards.clear();
  for (int i = 0; i < num_nodes; ++i) {
    for (int s = 0; s < num_shards; ++s) {
      shards.insert(ShardID(i, s));
    }
  }

  // try to shrink first num_nodes nodes
  // this is going to cause rebuilding stall as nodeset is only on first nodes
  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::READ_ONLY);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::REBUILDING_STALL |
                Impact::ImpactResult::WRITE_AVAILABILITY_LOSS,
            impact.result);
  ASSERT_TRUE(impact.internal_logs_affected);
}

TEST_F(SafetyAPIIntegrationTest, DrainWithSetWeight) {
  const size_t num_nodes = 5;
  const size_t num_shards = 2;

  Configuration::Nodes nodes;

  for (int i = 0; i < num_nodes; ++i) {
    nodes[i].generation = 1;
    nodes[i].addSequencerRole();
    nodes[i].addStorageRole(num_shards);
  }

  Configuration::Log logcfg;
  logcfg.rangeName = "test_range";
  logcfg.replicationFactor = 2;

  auto meta_configs =
      createMetaDataLogsConfig({0, 1, 2, 3, 4}, 2, NodeLocationScope::NODE);

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNumLogs(1)
                     .setNodes(nodes)
                     // switches on gossip
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .setLogConfig(logcfg)
                     .setMetaDataLogsConfig(meta_configs)
                     .create(num_nodes);

  for (const auto& it : cluster->getNodes()) {
    node_index_t idx = it.first;
    cluster->getNode(idx).waitUntilAvailable();
  }

  std::shared_ptr<Client> client = cluster->createClient();
  ClientImpl* client_impl = dynamic_cast<ClientImpl*>(client.get());

  write_test_records(client, LOG_ID, 10);

  ld_info("Waiting for metadata log writes to complete");
  cluster->waitForMetaDataLogWrites();

  SafetyChecker safety_checker(
      &client_impl->getProcessor(), 100, true, client_impl->getTimeout());
  ShardSet shards;

  for (int i = 0; i < 2; ++i) {
    for (int s = 0; s < num_shards; ++s) {
      shards.insert(ShardID(i, s));
    }
  }

  ShardAuthoritativeStatusMap shard_status{LSN_INVALID};
  int rv = cluster->getShardAuthoritativeStatusMap(shard_status);
  ASSERT_EQ(0, rv);

  // it is safe to drain 2 nodes as nodeset size is 5, replication is 2
  Impact impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::READ_ONLY);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::NONE, impact.result);

  // make nodes read only
  cluster->updateNodeAttributes(2, configuration::StorageState::READ_ONLY, 1);
  cluster->updateNodeAttributes(3, configuration::StorageState::READ_ONLY, 1);
  cluster->updateNodeAttributes(4, configuration::StorageState::READ_ONLY, 1);
  cluster->waitForMetaDataLogWrites();

  // now it is unsafe to drain first 2 nodes
  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::READ_ONLY);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::REBUILDING_STALL |
                Impact::ImpactResult::WRITE_AVAILABILITY_LOSS,
            impact.result);
  ASSERT_TRUE(impact.internal_logs_affected);
}

TEST_F(SafetyAPIIntegrationTest, DrainWithEventLogNotReadable) {
  const size_t num_nodes = 5;
  const size_t num_shards = 2;

  Configuration::Log logcfg;
  logcfg.rangeName = "test_range";
  logcfg.replicationFactor = 2;

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNumLogs(1)
                     // switches on gossip
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .setLogConfig(logcfg)
                     .setInternalLogConfig("event_log_deltas", createLog())
                     .setInternalLogConfig("event_log_snapshots", createLog())
                     .create(num_nodes);

  for (const auto& it : cluster->getNodes()) {
    node_index_t idx = it.first;
    cluster->getNode(idx).waitUntilAvailable();
  }

  std::shared_ptr<Client> client =
      cluster->createClient(std::chrono::seconds(10));
  ClientImpl* client_impl = dynamic_cast<ClientImpl*>(client.get());

  write_test_records(client, LOG_ID, 10);

  ld_info("Waiting for metadata log writes to complete");
  cluster->waitForMetaDataLogWrites();

  SafetyChecker safety_checker(
      &client_impl->getProcessor(), 100, true, client_impl->getTimeout());
  ShardSet shards;

  for (int i = 0; i < 3; ++i) {
    for (int s = 0; s < num_shards; ++s) {
      shards.insert(ShardID(i, s));
    }
  }

  ShardAuthoritativeStatusMap shard_status{LSN_INVALID};
  int rv = cluster->getShardAuthoritativeStatusMap(shard_status);
  ASSERT_EQ(0, rv);

  // it is unsafe to drain 3 nodes as replication is 3 for event log
  Impact impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::READ_ONLY);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::REBUILDING_STALL |
                Impact::ImpactResult::WRITE_AVAILABILITY_LOSS,
            impact.result);
  ASSERT_TRUE(impact.internal_logs_affected);

  // with event log replication factor 3, it is fine to loose two node
  cluster->getNode(num_nodes - 1).suspend();
  cluster->getNode(num_nodes - 2).suspend();

  shards.clear();
  shards.insert(ShardID(3, 0));

  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::READ_ONLY);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::NONE, impact.result);
}

TEST_F(SafetyAPIIntegrationTest, DisableReads) {
  const size_t num_nodes = 5;
  const size_t num_shards = 3;

  Configuration::Nodes nodes;

  for (int i = 0; i < num_nodes; ++i) {
    nodes[i].generation = 1;
    nodes[i].addSequencerRole();
    nodes[i].addStorageRole(num_shards);
  }

  Configuration::Log logcfg;
  logcfg.rangeName = "test_range";
  logcfg.replicationFactor = 3;

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNumLogs(2)
                     .setNodes(nodes)
                     // switches on gossip
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .setLogConfig(logcfg)
                     .setInternalLogConfig("event_log_deltas", createLog())
                     .setInternalLogConfig("event_log_snapshots", createLog())
                     .setInternalLogConfig("config_log_deltas", createLog())
                     .setInternalLogConfig("config_log_snapshots", createLog())
                     .create(num_nodes);

  for (const auto& it : cluster->getNodes()) {
    node_index_t idx = it.first;
    cluster->getNode(idx).waitUntilAvailable();
  }

  std::shared_ptr<Client> client = cluster->createClient();
  ClientImpl* client_impl = dynamic_cast<ClientImpl*>(client.get());

  write_test_records(client, LOG_ID, 10);
  write_test_records(client, LOG_ID2, 10);

  ld_info("Waiting for metadata log writes to complete");
  cluster->waitForMetaDataLogWrites();

  SafetyChecker safety_checker(
      &client_impl->getProcessor(), 100, true, client_impl->getTimeout());
  ShardSet shards;

  for (int i = 0; i < num_nodes; ++i) {
    shards.insert(ShardID(i, 0));
  }

  ShardAuthoritativeStatusMap shard_status{LSN_INVALID};
  int rv = cluster->getShardAuthoritativeStatusMap(shard_status);
  ASSERT_EQ(0, rv);

  // it is unsafe to stop all shards
  Impact impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::DISABLED);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::READ_AVAILABILITY_LOSS |
                Impact::ImpactResult::WRITE_AVAILABILITY_LOSS |
                Impact::ImpactResult::REBUILDING_STALL,
            impact.result);
  ASSERT_TRUE(impact.internal_logs_affected);

  // we have replication factor 3, NodeSet includes all nodes
  // it is safe to stop 2 node
  shards.clear();
  for (int i = 0; i < 2; ++i) {
    for (int s = 0; s < num_shards; ++s) {
      shards.insert(ShardID(i, s));
    }
  }

  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::DISABLED);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::NONE, impact.result);

  // stoping 3 same shards is unsafe
  shards.clear();
  for (int i = 0; i < 3; ++i) {
    shards.insert(ShardID(i, 2));
  }

  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::DISABLED);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::READ_AVAILABILITY_LOSS |
                Impact::ImpactResult::REBUILDING_STALL,
            impact.result);

  // stoping 3 different shards is fine
  shards.clear();
  shards.insert(ShardID(1, 1));
  shards.insert(ShardID(2, 2));
  shards.insert(ShardID(3, 3));
  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::DISABLED);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::NONE, impact.result);
  // Check that we don't set this on ImpactResult::NOME
  ASSERT_FALSE(impact.internal_logs_affected);
}

TEST_F(SafetyAPIIntegrationTest, SafetyMargin) {
  const size_t num_nodes = 5;
  const size_t num_shards = 5;

  Configuration::Nodes nodes;

  for (int i = 0; i < num_nodes; ++i) {
    nodes[i].generation = 1;
    nodes[i].addSequencerRole();
    nodes[i].addStorageRole(num_shards);
  }

  Configuration::Log logcfg;
  logcfg.rangeName = "test_range";
  logcfg.replicationFactor = 3;

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNumLogs(1)
                     .setNodes(nodes)
                     // switches on gossip
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .setLogConfig(logcfg)
                     .setInternalLogConfig("event_log_deltas", createLog())
                     .setInternalLogConfig("event_log_snapshots", createLog())
                     .setInternalLogConfig("config_log_deltas", createLog())
                     .setInternalLogConfig("config_log_snapshots", createLog())
                     .create(num_nodes);

  for (const auto& it : cluster->getNodes()) {
    node_index_t idx = it.first;
    cluster->getNode(idx).waitUntilAvailable();
  }

  std::shared_ptr<Client> client = cluster->createClient();
  ClientImpl* client_impl = dynamic_cast<ClientImpl*>(client.get());

  write_test_records(client, LOG_ID, 10);

  ld_info("Waiting for metadata log writes to complete");
  cluster->waitForMetaDataLogWrites();

  // double cluster size
  // cluster->expand(num_nodes);
  // for (const auto& it : cluster->getNodes()) {
  //   node_index_t idx = it.first;
  //   cluster->getNode(idx).waitUntilAvailable();
  // }

  // nodeset size is 3, first three nodes
  SafetyChecker safety_checker(
      &client_impl->getProcessor(), 100, true, client_impl->getTimeout());
  ShardSet shards;

  for (int i = 0; i < num_nodes; ++i) {
    for (int s = 0; s < num_shards; ++s) {
      shards.insert(ShardID(i, s));
    }
  }

  ShardAuthoritativeStatusMap shard_status{LSN_INVALID};
  int rv = cluster->getShardAuthoritativeStatusMap(shard_status);
  ASSERT_EQ(0, rv);

  // we have replication factor 3, NodeSet includes 3 nodes out of 6
  // it is safe to drain 1 node
  shards.clear();
  for (int i = 0; i < num_shards; ++i) {
    shards.insert(ShardID(1, i));
  }

  SafetyMargin safety;

  Impact impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::READ_ONLY, safety);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::NONE, impact.result);

  safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::DISABLED, safety);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::NONE, impact.result);

  // it is safe if we want to have 1 extra node
  safety[NodeLocationScope::NODE] = 1;
  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::READ_ONLY, safety);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::NONE, impact.result);

  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::DISABLED, safety);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::NONE, impact.result);

  // it is unsafe if we wantto have 2 extra nodes
  safety[NodeLocationScope::NODE] = 2;
  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::READ_ONLY, safety);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::REBUILDING_STALL |
                Impact::ImpactResult::WRITE_AVAILABILITY_LOSS,
            impact.result);
  ASSERT_TRUE(impact.internal_logs_affected);

  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::DISABLED, safety);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::READ_AVAILABILITY_LOSS |
                Impact::ImpactResult::REBUILDING_STALL |
                Impact::ImpactResult::WRITE_AVAILABILITY_LOSS,
            impact.result);
  ASSERT_TRUE(impact.internal_logs_affected);

  for (int i = 0; i < num_shards; ++i) {
    shards.insert(ShardID(2, i));
  }

  // it is fine to drain 2 nodes, without safety maring
  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::DISABLED);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::NONE, impact.result);

  // but not enough if we want one extra node
  safety[NodeLocationScope::NODE] = 1;
  impact = safety_checker.checkImpact(
      shard_status, shards, configuration::StorageState::DISABLED, safety);
  ld_info("IMPACT: %s", impact.toString().c_str());
  ASSERT_EQ(Impact::ImpactResult::READ_AVAILABILITY_LOSS |
                Impact::ImpactResult::REBUILDING_STALL |
                Impact::ImpactResult::WRITE_AVAILABILITY_LOSS,
            impact.result);
  ASSERT_TRUE(impact.internal_logs_affected);
}
