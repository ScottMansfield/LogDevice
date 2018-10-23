/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <unordered_map>

#include "logdevice/common/ClientBridge.h"
#include "logdevice/common/NodeID.h"
#include "logdevice/common/NodeSetAccessor.h"
#include "logdevice/common/NodeSetFinder.h"
#include "logdevice/common/Request.h"
#include "logdevice/common/RequestType.h"
#include "logdevice/common/SocketCallback.h"
#include "logdevice/common/Timer.h"
#include "logdevice/common/types_internal.h"
#include "logdevice/include/Client.h"
#include "logdevice/include/Err.h"

namespace facebook { namespace logdevice {

/**
 * @file TrimRequest is a client request responsible for broadcasting the
 *       TRIM message to all storage nodes.
 */

class TrimRequest : public Request {
 public:
  TrimRequest(ClientBridge* client,
              logid_t log_id,
              lsn_t trim_point,
              std::chrono::milliseconds client_timeout,
              trim_callback_t callback)
      : Request(RequestType::TRIM),
        client_(client),
        log_id_(log_id),
        trim_point_(trim_point),
        client_timeout_(client_timeout),
        callback_(callback),
        callback_helper_(this) {}

  ~TrimRequest() override;

  Execution execute() override;

  logid_t getLogID() const {
    return log_id_;
  }

  // see Request.h
  int getThreadAffinity(int /*nthreads*/) override {
    if (target_worker_.val_ >= 0) {
      return target_worker_.val_;
    }
    return -1;
  }

  /**
   * Called when Configuration::getLogByIDAsync() returns with the log config.
   */
  void onLogConfigAvailable(std::shared_ptr<LogsConfig::LogGroupNode> cfg);

  void onClientTimeout();

  void onReply(ShardID from, Status status);
  void onMessageSent(ShardID to, Status status);

  /**
   * Forces the TrimRequest to run on a specific Worker.
   */
  void setTargetWorker(worker_id_t id) {
    target_worker_ = id;
  }

  // Specify write token for specific request. Append will go through if
  // either per_request_token or per client token in ClientBridge will match
  // token specified in config. See Client.h addWriteToken() doc for more.
  void setPerRequestToken(std::unique_ptr<std::string> token) {
    per_request_token_ = std::move(token);
  }

  void bypassWriteTokenCheck() {
    bypass_write_token_check_ = true;
  }

  void bypassTailLSNCheck() {
    bypass_tail_lsn_check_ = true;
  }

 private:
  void fetchLogConfig();

  void onWriteTokenCheckDone();

  void checkForTrimPastTail();

  void onTrimPastTailCheckDone();

  // remove this object from Worker's runningTrimRequests_, causing it to be
  // deleted
  void deleteThis();

  // Determine the correct status based on server replies then invoke
  // finalize(Status)
  void finalize();
  // invoke callback_ with the supplied status and delete this instance
  void finalize(Status);

  // send a single TRIM message to the specified node
  int sendOneMessage(ShardID to);

  virtual std::unique_ptr<NodeSetFinder> makeNodeSetFinder();

  void initNodeSetFinder();

  void initStorageSetAccessor();

  /**
   * Initializes state and broadcasts initial messages to all servers.
   */
  void start(Status status);

  /**
   * Construct a TRIM_Message and send it to the node at index
   * idx. Can be used as shard_access callback in StorageSetAccessor
   * @returns SUCCESS if operation succeeded.
   *          PERMANENT_ERROR if unrecoverable error occurred.
   *          TRANSIENT_ERROR if an error occurred but the operation can be
   *            retried.
   */
  virtual StorageSetAccessor::SendResult sendTo(ShardID shard);

  virtual std::shared_ptr<ServerConfig> getConfig() const;

  virtual std::unique_ptr<StorageSetAccessor>
  makeStorageSetAccessor(const std::shared_ptr<ServerConfig>& config,
                         StorageSet shards,
                         ReplicationProperty minRep,
                         StorageSetAccessor::ShardAccessFunc shard_access,
                         StorageSetAccessor::CompletionFunc completion);

  ClientBridge* client_;
  logid_t log_id_;
  lsn_t trim_point_;
  const std::chrono::milliseconds client_timeout_;
  trim_callback_t callback_;
  std::unique_ptr<std::string> per_request_token_;
  WorkerCallbackHelper<TrimRequest> callback_helper_;

  // If not -1, run this request on specified Worker
  worker_id_t target_worker_{-1};

  bool bypass_write_token_check_ = false;
  bool bypass_tail_lsn_check_ = false;

  std::unique_ptr<NodeSetFinder> nodeset_finder_{nullptr};
  std::unique_ptr<StorageSetAccessor> storage_set_accessor_{nullptr};
};

// Wrapper instead of typedef to allow forward-declaring in Worker.h
struct TrimRequestMap {
  std::unordered_map<request_id_t,
                     std::unique_ptr<TrimRequest>,
                     request_id_t::Hash>
      map;
};

}} // namespace facebook::logdevice
