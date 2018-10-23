/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <memory>

#include <google/dense_hash_map>
#include <zookeeper/zookeeper.h>

#include "logdevice/common/ZookeeperClient.h"

namespace facebook { namespace logdevice {

class ZookeeperClientInMemory : public ZookeeperClientBase {
 public:
  /*
   * The (simplified) in-memory representation of the data in Zookeeper
   *   key - full path of node
   *   value - pair of value stored in the node and the stat of the node
   */
  using state_map_t =
      std::unordered_map<std::string, std::pair<std::string, zk::Stat>>;

  /**
   * ZookeeperClientInMemory emulates zookeeper using in-memory map
   * @param quorum           zookeeper quorum.
   *                         for testing any not null value could be used.
   * @param map              initial state of zookeeper.
   */
  ZookeeperClientInMemory(std::string quorum, state_map_t map);

  int state() override;

  int setData(const char* znode_path,
              const char* znode_value,
              int znode_value_size,
              int version,
              stat_completion_t completion,
              const void* data) override;

  int getData(const char* znode_path,
              data_completion_t completion,
              const void* data) override;

  int multiOp(int count,
              const zoo_op_t* ops,
              zoo_op_result_t* results,
              void_completion_t completion,
              const void* data) override;

  ~ZookeeperClientInMemory() override;

 private:
  bool parentsExist(const std::lock_guard<std::mutex>& lock,
                    const char* znode_path);

  int reconnect(zhandle_t* prev) override;
  std::shared_ptr<std::atomic<bool>> alive_;
  // Mutex protects `map_' and `callbacksGetData_'
  std::mutex mutex_;
  state_map_t map_;
  std::vector<std::thread> callbacksGetData_;

  //////// New API ////////
 public:
  int getData(std::string path, data_callback_t cb) override;
  int setData(std::string path,
              std::string data,
              stat_callback_t cb,
              zk::version_t base_version = -1) override;

  int multiOp(std::vector<zk::Op> ops, multi_op_callback_t cb) override;
};

}} // namespace facebook::logdevice
