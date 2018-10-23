/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "TRIM_onReceived.h"

#include <string>

#include "logdevice/common/Metadata.h"
#include "logdevice/common/PermissionChecker.h"
#include "logdevice/common/PrincipalIdentity.h"
#include "logdevice/common/Processor.h"
#include "logdevice/common/Sender.h"
#include "logdevice/common/UpdateableSecurityInfo.h"
#include "logdevice/common/configuration/Configuration.h"
#include "logdevice/common/protocol/TRIMMED_Message.h"
#include "logdevice/common/stats/Stats.h"
#include "logdevice/server/AuditLogFile.h"
#include "logdevice/server/ServerWorker.h"
#include "logdevice/server/locallogstore/LocalLogStore.h"
#include "logdevice/server/read_path/LogStorageStateMap.h"
#include "logdevice/server/storage_tasks/PerWorkerStorageTaskQueue.h"
#include "logdevice/server/storage_tasks/StorageTask.h"
#include "logdevice/server/storage_tasks/StorageThreadPool.h"

namespace facebook { namespace logdevice {

static void send_reply(const Address& to,
                       request_id_t client_rqid,
                       Status status,
                       shard_index_t shard) {
  TRIMMED_Header header = {client_rqid, status, shard};
  Worker::onThisThread()->sender().sendMessage(
      std::make_unique<TRIMMED_Message>(header), to);
}

namespace {
// A task to write the trim point to local log store
class WriteTrimMetadataTask : public StorageTask {
 public:
  explicit WriteTrimMetadataTask(logid_t log_id,
                                 lsn_t trim_point,
                                 const Address& reply_to,
                                 request_id_t client_rqid,
                                 std::string client_name,
                                 std::string client_address,
                                 PrincipalIdentity identity)
      : StorageTask(StorageTask::Type::WRITE_TRIM_METADATA),
        log_id_(log_id),
        trim_point_(trim_point),
        reply_to_(reply_to),
        client_rqid_(client_rqid),
        status_(E::OK),
        client_name_(std::move(client_name)),
        client_address_(std::move(client_address)),
        identity_(std::move(identity)) {}

  void execute() override {
    LocalLogStore& store = storageThreadPool_->getLocalLogStore();
    LogStorageStateMap& map =
        storageThreadPool_->getProcessor().getLogStorageStateMap();

    TrimMetadata trim_metadata{trim_point_};
    LocalLogStore::WriteOptions options;
    int rv = store.updateLogMetadata(log_id_, trim_metadata, options);
    if (rv != 0) {
      // if local log store already contained a trim point with a higher LSN,
      // report it as a success to the client
      status_ = (err == E::UPTODATE ? E::OK : E::FAILED);
      return;
    }
    auto processor =
        checked_downcast<ServerProcessor*>(&storageThreadPool_->getProcessor());
    log_trim_movement(*processor,
                      store,
                      log_id_,
                      trim_point_,
                      client_name_,
                      client_address_,
                      identity_);

    LogStorageState* log_state =
        map.insertOrGet(log_id_, storageThreadPool_->getShardIdx());
    if (log_state == nullptr) {
      status_ = E::FAILED;
      return;
    }

    log_state->updateTrimPoint(trim_point_);
    durability_ = Durability::SYNC_WRITE;
  }

  Durability durability() const override {
    return durability_;
  }

  void onDone() override {
    send_reply(
        reply_to_, client_rqid_, status_, storageThreadPool_->getShardIdx());
  }

  void onDropped() override {
    send_reply(
        reply_to_, client_rqid_, E::FAILED, storageThreadPool_->getShardIdx());
  }

 private:
  logid_t log_id_;
  lsn_t trim_point_;
  Address reply_to_;
  request_id_t client_rqid_;
  Status status_;
  Durability durability_ = Durability::INVALID;
  std::string client_name_;
  std::string client_address_;
  PrincipalIdentity identity_;
};
} // namespace

static Message::Disposition
send_reply(TRIM_Message* msg,
           const Address& from,
           shard_index_t shard_idx,
           PermissionCheckStatus permission_status) {
  const TRIM_Header& header = msg->getHeader();
  ServerWorker* worker = ServerWorker::onThisThread();

  if (permission_status == PermissionCheckStatus::DENIED) {
    send_reply(from, header.client_rqid, E::ACCESS, shard_idx);
    RATELIMIT_ERROR(std::chrono::seconds(1),
                    3,
                    "TRIM_Message from %s for log %lu failed with "
                    "E::ACCESS because client does not have the required "
                    "permissions to complete the request",
                    Sender::describeConnection(from).c_str(),
                    header.log_id.val_);
    return Message::Disposition::NORMAL;
  } else if (permission_status == PermissionCheckStatus::NOTREADY) {
    RATELIMIT_INFO(std::chrono::seconds(1),
                   3,
                   "Got TRIM_Message from %s for log %lu but "
                   "permission checker is not yet loaded. "
                   "Responding with E::NOTREADY",
                   Sender::describeConnection(from).c_str(),
                   header.log_id.val_);
    send_reply(from, header.client_rqid, E::AGAIN, shard_idx);
    return Message::Disposition::NORMAL;
  } else if (permission_status == PermissionCheckStatus::SYSLIMIT) {
    RATELIMIT_INFO(std::chrono::seconds(1),
                   3,
                   "Got TRIM_Message from %s for log %lu but "
                   "there is not enough background threads. "
                   "Responding with E::SYSLIMIT",
                   Sender::describeConnection(from).c_str(),
                   header.log_id.val_);
    send_reply(from, header.client_rqid, E::SYSLIMIT, shard_idx);
    return Message::Disposition::NORMAL;
  } else if (permission_status == PermissionCheckStatus::NOTFOUND) {
    RATELIMIT_INFO(std::chrono::seconds(1),
                   3,
                   "Got TRIM_Message from %s for log %lu but "
                   "log is not present in config. "
                   "Responding with E::NOTFOUND",
                   Sender::describeConnection(from).c_str(),
                   header.log_id.val_);
    send_reply(from, header.client_rqid, E::NOTFOUND, shard_idx);
    return Message::Disposition::NORMAL;
  }

  if (!worker->isAcceptingWork()) {
    ld_debug("Ignoring TRIM message: not accepting more work");
    send_reply(from, header.client_rqid, E::SHUTDOWN, shard_idx);
    return Message::Disposition::NORMAL;
  }

  WORKER_LOG_STAT_INCR(header.log_id, trim_received);

  if (header.log_id == LOGID_INVALID || header.trim_point == LSN_INVALID ||
      !epoch_valid_or_unset(lsn_to_epoch(header.trim_point))) {
    ld_error("Received an invalid TRIM message from %s: "
             "log id %lu, trim point %lu",
             Sender::describeConnection(from).c_str(),
             header.log_id.val_,
             header.trim_point);

    send_reply(from, header.client_rqid, E::INVALID_PARAM, shard_idx);
    return Message::Disposition::NORMAL;
  }

  // queue a task that'll write trim_point to the log store, update the
  // state map and send a reply to the client
  auto task = std::make_unique<WriteTrimMetadataTask>(
      header.log_id,
      header.trim_point,
      from,
      header.client_rqid,
      Sender::describeConnection(from),
      worker->sender().getSockaddr(from).toStringNoPort(),
      *worker->sender().getPrincipal(from));
  worker->getStorageTaskQueueForShard(shard_idx)->putTask(std::move(task));

  return Message::Disposition::NORMAL;
}

Message::Disposition TRIM_onReceived(TRIM_Message* msg, const Address& from) {
  if (!from.isClientAddress()) {
    ld_error("Received TRIM message from non-client %s",
             Sender::describeConnection(from).c_str());
    err = E::PROTO;
    return Message::Disposition::ERROR;
  }

  const TRIM_Header& header = msg->getHeader();
  ServerWorker* worker = ServerWorker::onThisThread();

  auto scfg = worker->getServerConfig();
  const shard_size_t n_shards = scfg->getNumShards();
  shard_index_t shard_idx = header.shard;
  if (shard_idx >= n_shards) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    10,
                    "Got TRIM message from client %s with "
                    "invalid shard %u, this node only has %u shards",
                    Sender::describeConnection(from).c_str(),
                    shard_idx,
                    n_shards);
    return Message::Disposition::NORMAL;
  }

  Processor* processor = worker->processor_;
  if (!processor->runningOnStorageNode()) {
    ld_warning("Received a TRIM message from %s, but not a storage node",
               Sender::describeConnection(from).c_str());

    send_reply(from, header.client_rqid, E::NOTSTORAGE, shard_idx);
    return Message::Disposition::NORMAL;
  }

  auto permission_checker =
      Worker::onThisThread()
          ->processor_->security_info_->getPermissionChecker();
  // If the processor owns a PermissionChecker then permission checking is
  // enabled.
  if (permission_checker) {
    const PrincipalIdentity* principal =
        Worker::onThisThread()->sender().getPrincipal(from);
    if (principal == nullptr) {
      ld_critical("TRIM_Message from %s for log %lu failed because "
                  "there is no Principal Associated with from %s",
                  Sender::describeConnection(from).c_str(),
                  header.log_id.val_,
                  Sender::describeConnection(from).c_str());
      // This should never happen. We are invoking onReceived from the socket
      // that has already performed the hello/ack handshake.
      ld_check(false);
      err = E::ACCESS;
      return Message::Disposition::ERROR;
    }

    ld_check(permission_checker);
    permission_checker->isAllowed(
        ACTION::TRIM,
        *principal,
        header.log_id,
        [msg, from, shard_idx](
            PermissionCheckStatus permission_status) mutable {
          send_reply(std::unique_ptr<TRIM_Message>(msg).get(),
                     from,
                     shard_idx,
                     permission_status);
        });
    return Message::Disposition::KEEP;
  } else {
    return send_reply(msg, from, shard_idx, PermissionCheckStatus::NONE);
  }
}
}} // namespace facebook::logdevice
