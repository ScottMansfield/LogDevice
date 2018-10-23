/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "SEALED_Message.h"

#include <folly/Memory.h>

#include "logdevice/common/LogRecoveryRequest.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/protocol/Compatibility.h"
#include "logdevice/common/protocol/ProtocolReader.h"
#include "logdevice/common/protocol/ProtocolWriter.h"
#include "logdevice/common/stats/Stats.h"

namespace facebook { namespace logdevice {

size_t SEALED_Header::getExpectedSize(uint16_t proto) {
  if (proto < Compatibility::TAIL_RECORD_IN_SEALED) {
    return offsetof(SEALED_Header, num_tail_records);
  } else {
    return sizeof(SEALED_Header);
  }
}

void SEALED_Message::serialize(ProtocolWriter& writer) const {
  ld_check(header_.lng_list_size == epoch_lng_.size());
  ld_check(header_.lng_list_size == epoch_offset_map_.size());
  ld_check(header_.lng_list_size == last_timestamp_.size());
  ld_check(header_.lng_list_size == max_seen_lsn_.size());
  writer.write(&header_, SEALED_Header::getExpectedSize(writer.proto()));
  writer.writeVector(epoch_lng_);
  writer.write(seal_);

  if (writer.proto() >= Compatibility::OFFSET_MAP_SUPPORT_IN_SEALED_MSG) {
    writer.writeVectorOfSerializable(epoch_offset_map_);
  } else {
    // Serialize the old format.
    std::vector<uint64_t> epoch_offset_map(epoch_offset_map_.size());
    for (size_t i = 0; i < epoch_offset_map_.size(); ++i) {
      epoch_offset_map[i] =
          epoch_offset_map_[i].getCounter(CounterType::BYTE_OFFSET);
    }
    writer.writeVector(epoch_offset_map);
  }

  writer.writeVector(last_timestamp_);

  writer.protoGate(Compatibility::TAIL_RECORD_IN_SEALED);
  writer.writeVector(max_seen_lsn_);

  for (const auto& tr : tail_records_) {
    tr.serialize(writer);
  }
}

MessageReadResult SEALED_Message::deserialize(ProtocolReader& reader) {
  SEALED_Header header;
  // Defaults for old protocols
  header.shard = -1;
  header.num_tail_records = -1;
  reader.read(&header, SEALED_Header::getExpectedSize(reader.proto()));

  std::vector<lsn_t> epoch_lng(header.lng_list_size);
  Seal seal;
  std::vector<OffsetMap> epoch_offset_map(header.lng_list_size);
  std::vector<uint64_t> last_timestamp(header.lng_list_size, 0);
  std::vector<lsn_t> max_seen_lsn(header.lng_list_size, LSN_INVALID);

  reader.readVector(&epoch_lng);
  reader.read(&seal);

  if (reader.proto() >= Compatibility::OFFSET_MAP_SUPPORT_IN_SEALED_MSG) {
    reader.readVectorOfSerializable(&epoch_offset_map, header.lng_list_size);
  } else {
    // Read the old format.
    std::vector<uint64_t> epoch_offset_map_legacy(
        header.lng_list_size, BYTE_OFFSET_INVALID);
    reader.readVector(&epoch_offset_map_legacy);
    for (size_t i = 0; i < epoch_offset_map_legacy.size(); ++i) {
      epoch_offset_map[i].setCounter(
          CounterType::BYTE_OFFSET, epoch_offset_map_legacy[i]);
    }
  }

  reader.readVector(&last_timestamp);
  reader.protoGate(Compatibility::TAIL_RECORD_IN_SEALED);

  reader.readVector(&max_seen_lsn);

  std::vector<TailRecord> tail_records;
  if (header.num_tail_records > 0) {
    tail_records.resize(header.num_tail_records);
    for (auto& tr : tail_records) {
      // currently we don't send payloads with SEALED message, so zero
      // copy here is not relevant
      tr.deserialize(reader, /*zero_copy*/ true);
    }
  }

  return reader.result([&] {
    return new SEALED_Message(header,
                              std::move(epoch_lng),
                              seal,
                              std::move(last_timestamp),
                              std::move(epoch_offset_map),
                              std::move(max_seen_lsn),
                              std::move(tail_records));
  });
}

Message::Disposition SEALED_Message::onReceived(const Address& from) {
  Worker* worker = Worker::onThisThread();

  auto& rqmap = worker->runningLogRecoveries().map;
  auto it = rqmap.find(header_.log_id);
  if (it == rqmap.end()) {
    return Disposition::NORMAL;
  }

  auto scfg = worker->getServerConfig();
  auto* node = scfg->getNode(from.id_.node_.index());
  if (!node || !node->isReadableStorageNode()) {
    RATELIMIT_INFO(std::chrono::seconds(10),
                   10,
                   "Got a SEALED message for log %lu from %s but this node is "
                   "not a storage node. Ignoring.",
                   header_.log_id.val_,
                   Sender::describeConnection(from).c_str());
    return Disposition::NORMAL;
  }

  const shard_size_t n_shards = node->getNumShards();
  ld_check(n_shards > 0); // We already checked we are a storage node.
  shard_index_t shard_idx = header_.shard;
  if (shard_idx >= n_shards) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    10,
                    "Got SEALED message from client %s with invalid shard %u, "
                    "this node only has %u shards",
                    Sender::describeConnection(from).c_str(),
                    shard_idx,
                    n_shards);
    return Message::Disposition::NORMAL;
  }

  it->second->onSealReply(ShardID(from.id_.node_.index(), shard_idx), *this);

  return Disposition::NORMAL;
}

void SEALED_Message::createAndSend(const Address& to,
                                   logid_t log_id,
                                   shard_index_t shard_idx,
                                   epoch_t seal_epoch,
                                   Status status,
                                   std::vector<lsn_t> lng_list,
                                   Seal seal,
                                   std::vector<OffsetMap> epoch_offset_map,
                                   std::vector<uint64_t> last_timestamp,
                                   std::vector<lsn_t> max_seen_lsn,
                                   std::vector<TailRecord> tail_records) {
  ld_check(lng_list.size() == last_timestamp.size());
  ld_check(lng_list.size() == epoch_offset_map.size());
  SEALED_Header header;
  header.log_id = log_id;
  header.shard = shard_idx;
  header.seal_epoch = seal_epoch;
  header.status = status;

  // header.num_tail_records set by constructor

  int rv = Worker::onThisThread()->sender().sendMessage(
      std::make_unique<SEALED_Message>(header,
                                       std::move(lng_list),
                                       seal,
                                       std::move(last_timestamp),
                                       std::move(epoch_offset_map),
                                       std::move(max_seen_lsn),
                                       std::move(tail_records)),
      to);

  if (rv != 0) {
    RATELIMIT_WARNING(std::chrono::seconds(10),
                      10,
                      "Failed to send SEALED reply for log id %lu, epoch %u, "
                      "seal status %s to %s: %s.",
                      header.log_id.val_,
                      header.seal_epoch.val_,
                      error_name(header.status),
                      Sender::describeConnection(to).c_str(),
                      error_name(header.status));
    WORKER_STAT_INCR(sealed_reply_failed_to_send);
  }
}

void SEALED_Message::onSent(Status status, const Address& to) const {
  if (status != E::OK) {
    RATELIMIT_WARNING(std::chrono::seconds(10),
                      10,
                      "Failed to send SEALED reply for log id %lu, epoch %u, "
                      "seal status %s to %s: %s",
                      header_.log_id.val_,
                      header_.seal_epoch.val_,
                      error_name(header_.status),
                      Sender::describeConnection(to).c_str(),
                      error_name(status));
    WORKER_STAT_INCR(sealed_reply_failed_to_send);
  }
}

std::string SEALED_Message::toString() const {
  std::string ret = folly::sformat("log_id: {}, seal_epoch: {}, status: {}, "
                                   "lng_list_size: {}, seal_: {}, epoch_lng_:",
                                   header_.log_id.val_,
                                   header_.seal_epoch.val_,
                                   error_name(header_.status),
                                   header_.lng_list_size,
                                   seal_.toString());
  for (const lsn_t lsn : epoch_lng_) {
    ret += " " + lsn_to_string(lsn);
  }
  // TODO: Add last_timestamp_ and epoch_offset_map_.
  return ret;
}
}} // namespace facebook::logdevice
