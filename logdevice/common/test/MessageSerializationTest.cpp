/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <cstring>
#include <functional>

#include <folly/Memory.h>
#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>

#include "event2/buffer.h"
#include "logdevice/common/Metadata.h"
#include "logdevice/common/Processor.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/libevent/compat.h"
#include "logdevice/common/protocol/APPEND_Message.h"
#include "logdevice/common/protocol/CLEAN_Message.h"
#include "logdevice/common/protocol/DELETE_Message.h"
#include "logdevice/common/protocol/GET_EPOCH_RECOVERY_METADATA_Message.h"
#include "logdevice/common/protocol/GET_EPOCH_RECOVERY_METADATA_REPLY_Message.h"
#include "logdevice/common/protocol/HELLO_Message.h"
#include "logdevice/common/protocol/MessageDeserializers.h"
#include "logdevice/common/protocol/MessageTypeNames.h"
#include "logdevice/common/protocol/ProtocolReader.h"
#include "logdevice/common/protocol/ProtocolWriter.h"
#include "logdevice/common/protocol/RECORD_Message.h"
#include "logdevice/common/protocol/SEALED_Message.h"
#include "logdevice/common/protocol/SHUTDOWN_Message.h"
#include "logdevice/common/protocol/STARTED_Message.h"
#include "logdevice/common/protocol/START_Message.h"
#include "logdevice/common/protocol/STOP_Message.h"
#include "logdevice/common/protocol/STORE_Message.h"
#include "logdevice/common/request_util.h"
#include "logdevice/common/test/TestUtil.h"
#include "logdevice/common/util.h"

namespace arg = std::placeholders;

namespace facebook { namespace logdevice {

template <typename SomeMessage, typename CheckFn, typename ExpectedHexFn>
void DO_TEST(const SomeMessage& m,
             CheckFn&& check,
             uint16_t min_proto,
             uint16_t max_proto,
             ExpectedHexFn&& expected_fn,
             std::function<Message::deserializer_t> deserializer,
             Status proto_writer_status = E::OK) {
  ld_check(min_proto >= Compatibility::MIN_PROTOCOL_SUPPORTED);
  ld_check(max_proto <= Compatibility::MAX_PROTOCOL_SUPPORTED);

  for (uint16_t proto = min_proto; proto <= max_proto; ++proto) {
    struct evbuffer* evbuf = LD_EV(evbuffer_new)();
    ProtocolWriter writer(m.type_, evbuf, proto);
    m.serialize(writer);
    ssize_t sz = writer.result();
    ASSERT_EQ(writer.status(), proto_writer_status);
    if (proto_writer_status != E::OK) {
      continue;
    }
    ASSERT_GT(sz, 0);
    std::string serialized(sz, '0');
    ASSERT_EQ(sz, LD_EV(evbuffer_copyout)(evbuf, &serialized[0], sz));
    std::string found_hex = hexdump_buf(&serialized[0], sz);
    std::string expected_hex = expected_fn(proto);
    if (expected_hex.empty()) {
      ld_info("serialized %s for protocol %u: %s",
              messageTypeNames[m.type_].c_str(),
              (uint32_t)proto,
              found_hex.c_str());
    } else {
      EXPECT_EQ(expected_hex, found_hex);
    }
    std::unique_ptr<Message> found;

    // If a custom deserializer was not passed in, use the default for the
    // message type
    if (!deserializer) {
      deserializer = messageDeserializers[m.type_];
    }
    ProtocolReader reader(m.type_, evbuf, sz, proto);
    found = deserializer(reader).msg;
    ASSERT_NE(nullptr, found);
    ASSERT_EQ(0, LD_EV(evbuffer_get_length)(evbuf));
    SomeMessage* found_sub = dynamic_cast<SomeMessage*>(found.get());
    ASSERT_NE(nullptr, found_sub);
    check(*found_sub, proto);
    LD_EV(evbuffer_free)(evbuf);
  }
}

// TODO: now that Payload has toString(), we don't need this helper.
static std::string getPayload(const Payload& p) {
  return p.toString();
}

static std::string getPayload(const PayloadHolder& h) {
  struct evbuffer* evbuf = LD_EV(evbuffer_new)();
  ProtocolWriter writer(
      MessageType::STORE, evbuf, Compatibility::MAX_PROTOCOL_SUPPORTED);
  h.serialize(writer);
  ssize_t sz = writer.result();
  EXPECT_GE(sz, 0);
  std::string s(sz, '\0');
  EXPECT_EQ(sz, LD_EV(evbuffer_remove)(evbuf, &s[0], sz));
  EXPECT_EQ(0, LD_EV(evbuffer_get_length)(evbuf));
  LD_EV(evbuffer_free)(evbuf);
  return s;
}

class MessageSerializationTest : public ::testing::Test {
 public:
  MessageSerializationTest() {
    dbg::assertOnData = true;
  }
  void checkSTORE(const STORE_Message& sent,
                  const STORE_Message& recv,
                  uint16_t proto) {
    ASSERT_EQ(sent.header_.rid, recv.header_.rid);
    ASSERT_EQ(sent.header_.timestamp, recv.header_.timestamp);
    ASSERT_EQ(sent.header_.last_known_good, recv.header_.last_known_good);
    ASSERT_EQ(sent.header_.wave, recv.header_.wave);
    ASSERT_EQ(sent.header_.nsync, recv.header_.nsync);
    ASSERT_EQ(sent.header_.copyset_offset, recv.header_.copyset_offset);
    ASSERT_EQ(sent.header_.copyset_size, recv.header_.copyset_size);
    ASSERT_EQ(sent.header_.flags, recv.header_.flags);
    ASSERT_EQ(sent.header_.timeout_ms, recv.header_.timeout_ms);
    ASSERT_EQ(sent.header_.sequencer_node_id, recv.header_.sequencer_node_id);
    ASSERT_EQ(sent.extra_.recovery_id, recv.extra_.recovery_id);
    ASSERT_EQ(sent.extra_.recovery_epoch, recv.extra_.recovery_epoch);
    ASSERT_EQ(sent.copyset_.size(), recv.copyset_.size());
    ASSERT_EQ(sent.extra_.rebuilding_version, recv.extra_.rebuilding_version);
    ASSERT_EQ(sent.extra_.rebuilding_wave, recv.extra_.rebuilding_wave);
    ASSERT_EQ(sent.extra_.offset_within_epoch, recv.extra_.offset_within_epoch);

    if (sent.header_.wave > 1) {
      ASSERT_EQ(sent.extra_.first_amendable_offset,
                recv.extra_.first_amendable_offset);
    } else {
      ASSERT_EQ(COPYSET_SIZE_MAX, recv.extra_.first_amendable_offset);
    }

    for (size_t i = 0; i < sent.copyset_.size(); ++i) {
      ASSERT_EQ(sent.copyset_[i].destination, recv.copyset_[i].destination);
      ASSERT_EQ(sent.copyset_[i].origin, recv.copyset_[i].origin);
    }

    ASSERT_EQ(sent.block_starting_lsn_, recv.block_starting_lsn_);
    ASSERT_EQ(sent.optional_keys_.size(), recv.optional_keys_.size());
    for (const auto& key_pair : sent.optional_keys_) {
      ASSERT_NE(
          recv.optional_keys_.find(key_pair.first), recv.optional_keys_.end());
      ASSERT_EQ(
          recv.optional_keys_.find(key_pair.first)->second, key_pair.second);
    }

    if (proto >= Compatibility::STORE_E2E_TRACING_SUPPORT) {
      ASSERT_EQ(sent.e2e_tracing_context_, recv.e2e_tracing_context_);
    } else {
      ASSERT_TRUE(recv.e2e_tracing_context_.empty());
    }

    if (proto >= Compatibility::OFFSET_MAP_SUPPORT &&
        sent.header_.flags & STORE_Header::OFFSET_MAP) {
      ASSERT_EQ(
          sent.extra_.offsets_within_epoch, recv.extra_.offsets_within_epoch);
    }

    ASSERT_EQ(getPayload(*sent.payload_), getPayload(*recv.payload_));
  }

  void checkAPPEND(const APPEND_Message& m,
                   const APPEND_Message& m2,
                   uint16_t proto) {
    ASSERT_EQ(m.header_.rqid, m2.header_.rqid);
    ASSERT_EQ(m.header_.logid, m2.header_.logid);
    ASSERT_EQ(m.header_.seen, m2.header_.seen);
    ASSERT_EQ(m.header_.timeout_ms, m2.header_.timeout_ms);
    ASSERT_EQ(m.header_.flags, m2.header_.flags);
    ASSERT_EQ(m.attrs_.optional_keys, m2.attrs_.optional_keys);
    ASSERT_EQ(m.attrs_.counters.hasValue(), m2.attrs_.counters.hasValue());
    if (m.attrs_.counters.hasValue()) {
      ASSERT_EQ(m.attrs_.counters->size(), m2.attrs_.counters->size());
      for (auto counter : m.attrs_.counters.value()) {
        auto map =
            const_cast<std::map<uint8_t, int64_t>&>(m2.attrs_.counters.value());
        ASSERT_EQ(counter.second, map[counter.first]);
      }
    }

    if (proto >= Compatibility::APPEND_E2E_TRACING_SUPPORT) {
      ASSERT_EQ(m.e2e_tracing_context_, m2.e2e_tracing_context_);
    } else {
      ASSERT_TRUE(m2.e2e_tracing_context_.empty());
    }

    ASSERT_EQ(getPayload(m.payload_), getPayload(m2.payload_));
  }

  void checkRECORD(const RECORD_Message& m,
                   const RECORD_Message& m2,
                   uint16_t /*proto*/) {
    ASSERT_EQ(m.header_.log_id, m2.header_.log_id);
    ASSERT_EQ(m.header_.read_stream_id, m2.header_.read_stream_id);
    ASSERT_EQ(m.header_.lsn, m2.header_.lsn);
    ASSERT_EQ(m.header_.timestamp, m2.header_.timestamp);
    ASSERT_EQ(m.header_.flags, m2.header_.flags);

    ASSERT_EQ(getPayload(m.payload_), getPayload(m2.payload_));

    ld_check(m.extra_metadata_ != nullptr);
    ld_check(m2.extra_metadata_ != nullptr);

    ASSERT_EQ(m.extra_metadata_->header.last_known_good,
              m2.extra_metadata_->header.last_known_good);
    ASSERT_EQ(m.extra_metadata_->header.wave, m2.extra_metadata_->header.wave);
    ASSERT_EQ(m.extra_metadata_->header.copyset_size,
              m2.extra_metadata_->header.copyset_size);
    ASSERT_EQ(m.extra_metadata_->copyset, m2.extra_metadata_->copyset);
  }

  void checkSEALED(const SEALED_Message& m,
                   const SEALED_Message& m2,
                   uint16_t proto) {
    ASSERT_EQ(m.header_.log_id, m2.header_.log_id);
    ASSERT_EQ(m.header_.seal_epoch, m2.header_.seal_epoch);
    ASSERT_EQ(m.header_.lng_list_size, m2.header_.lng_list_size);
    ASSERT_EQ(m.header_.shard, m2.header_.shard);

    if (proto < Compatibility::TAIL_RECORD_IN_SEALED) {
      ASSERT_EQ(-1, m2.header_.num_tail_records);
      ASSERT_EQ(0, m2.tail_records_.size());
      ASSERT_EQ(std::vector<lsn_t>(m.header_.lng_list_size, LSN_INVALID),
                m2.max_seen_lsn_);
    } else {
      ASSERT_EQ(m.header_.num_tail_records, m2.header_.num_tail_records);
      ASSERT_EQ(m2.header_.num_tail_records, m2.tail_records_.size());
      ASSERT_EQ(m.max_seen_lsn_, m2.max_seen_lsn_);
    }

    ASSERT_EQ(m.epoch_lng_, m2.epoch_lng_);
    ASSERT_EQ(m.seal_, m2.seal_);
    ASSERT_EQ(m.last_timestamp_, m2.last_timestamp_);
    ASSERT_EQ(m.epoch_offset_map_, m2.epoch_offset_map_);

    if (proto < Compatibility::TAIL_RECORD_IN_SEALED) {
      ASSERT_EQ(0, m2.tail_records_.size());
    } else {
      ASSERT_EQ(m.tail_records_.size(), m2.tail_records_.size());
      for (int i = 0; i < m.tail_records_.size(); ++i) {
        bool same = m.tail_records_[i].sameContent(m2.tail_records_[i]);
        ASSERT_TRUE(same);
      }
    }
  }

  void checkSTARTED(const STARTED_Message& m,
                    const STARTED_Message& m2,
                    uint16_t /*proto*/) {
    ASSERT_EQ(m.header_.log_id, m2.header_.log_id);
    ASSERT_EQ(m.header_.read_stream_id, m2.header_.read_stream_id);
    ASSERT_EQ(m.header_.status, m2.header_.status);
    ASSERT_EQ(m.header_.filter_version, m2.header_.filter_version);
    ASSERT_EQ(m.header_.last_released_lsn, m2.header_.last_released_lsn);
  }

  void checkCLEAN(const CLEAN_Message& m,
                  const CLEAN_Message& m2,
                  uint16_t /*proto*/) {
    ASSERT_EQ(m.header_.log_id, m2.header_.log_id);
    ASSERT_EQ(m.header_.epoch, m2.header_.epoch);
    ASSERT_EQ(m.header_.recovery_id, m2.header_.recovery_id);
    ASSERT_EQ(m.header_.epoch_size, m2.header_.epoch_size);
  }

  void checkSHUTDOWN(const SHUTDOWN_Message& m,
                     const SHUTDOWN_Message& m2,
                     uint16_t /*proto*/) {
    ASSERT_EQ(m.header_.status, m2.header_.status);
    ASSERT_EQ(m.header_.serverInstanceId, m2.header_.serverInstanceId);
  }

  void
  checkGetEpochRecoveryMetadata(const GET_EPOCH_RECOVERY_METADATA_Message& m1,
                                const GET_EPOCH_RECOVERY_METADATA_Message& m2,
                                uint16_t proto) {
    ASSERT_EQ(m1.header_.log_id, m2.header_.log_id);
    ASSERT_EQ(m1.header_.purge_to, m2.header_.purge_to);
    ASSERT_EQ(m1.header_.start, m2.header_.start);
    ASSERT_EQ(m1.header_.flags, m2.header_.flags);
    ASSERT_EQ(m1.header_.shard, m2.header_.shard);
    ASSERT_EQ(m1.header_.purging_shard, m2.header_.purging_shard);

    if (proto < Compatibility::GET_EPOCH_RECOVERY_RANGE_SUPPORT) {
      ASSERT_EQ(m1.header_.start, m2.header_.end);
      ASSERT_EQ(REQUEST_ID_INVALID, m2.header_.id);
    } else {
      ASSERT_EQ(m1.header_.end, m2.header_.end);
      ASSERT_EQ(m1.header_.id, m2.header_.id);
    }
  }

  void checkGetEpochRecoveryMetadataReply(
      const GET_EPOCH_RECOVERY_METADATA_REPLY_Message& m1,
      const GET_EPOCH_RECOVERY_METADATA_REPLY_Message& m2,
      uint16_t /*unused*/) {
    ASSERT_EQ(m1.header_.log_id, m2.header_.log_id);
    ASSERT_EQ(m1.header_.purge_to, m2.header_.purge_to);
    ASSERT_EQ(m1.header_.start, m2.header_.start);
    ASSERT_EQ(m1.header_.flags, m2.header_.flags);
    ASSERT_EQ(m1.header_.status, m2.header_.status);
    ASSERT_EQ(m1.header_.shard, m2.header_.shard);
    ASSERT_EQ(m1.header_.purging_shard, m2.header_.purging_shard);
    ASSERT_EQ(m1.header_.end, m2.header_.end);
    ASSERT_EQ(m1.header_.id, m2.header_.id);
    ASSERT_EQ(m1.header_.num_non_empty_epochs, m2.header_.num_non_empty_epochs);
    ASSERT_EQ(m1.status_, m2.status_);
    ASSERT_EQ(m1.metadata_, m2.metadata_);
  }
};

namespace {
// Helper factory for STORE messages, including their serialized forms.
// Provides a basic STORE header so that tests don't need to copy-paste it.
struct TestStoreMessageFactory {
  TestStoreMessageFactory() {
    header_ = STORE_Header{
        RecordID(0xc5e58b03be6504ce, logid_t(0x0fbc3a81c75251e2)),
        0xeb4925bfde9dec8e,
        esn_t(0xebecb8bb),
        0x00000001, // wave
        0,          // flags
        1,
        2,
        3, // copyset size
        0xb9b421a5,
        NodeID(0x508b, 0xa554),
    };
    cs_.assign({{ShardID(1, 0), ClientID(4)},
                {ShardID(2, 0), ClientID(5)},
                {ShardID(3, 0), ClientID(6)}});
    payload_ = "hi";
  }

  void setWave(uint32_t wave) {
    header_.wave = wave;
  }

  void setFlags(STORE_flags_t flags) {
    header_.flags = flags;
  }

  void setExtra(STORE_Extra extra, std::string serialized) {
    extra_ = std::move(extra);
    extra_serialized_ = std::move(serialized);
  }

  void setKey(std::map<KeyType, std::string> optional_keys,
              std::string serialized) {
    optional_keys_ = std::move(optional_keys);
    key_serialized_ = std::move(serialized);
  }

  void setE2ETracingContext(std::string tracing_context,
                            std::string serialized) {
    e2e_tracing_context_ = std::move(tracing_context);
    e2e_tracing_context_serialized_ = std::move(serialized);
  }

  STORE_Message message() const {
    return STORE_Message(
        header_,
        cs_.data(),
        header_.copyset_offset,
        0,
        extra_,
        optional_keys_,
        std::make_shared<PayloadHolder>(
            Payload(payload_.data(), payload_.size()), PayloadHolder::UNOWNED),
        false,
        e2e_tracing_context_);
  }

  template <typename IntType>
  static std::string hex(IntType val) {
    std::string rv;
    for (size_t i = 0; i < sizeof(val); ++i) {
      static const char digits[] = "0123456789ABCDEF";
      // Little endian
      rv += digits[(val / 16) % 16];
      rv += digits[val % 16];
      val /= 256;
    }
    return rv;
  }

  std::string serialized(uint16_t proto) const {
    std::string rv = "CE0465BE038BE5C5E25152C7813ABC0F" // rid
                     "8EEC9DDEBF2549EB"                 // timestamp
                     "BBB8ECEB"                         // last_known_good
        + hex(header_.wave) + hex(header_.flags) +
        "010203"   // nsync, copyset offset and size
        "A521B4B9" // timeout_ms
        "54A58B50" // sequencer_node_id
        ;

    // Take into account OffsetMap bits
    if (proto >= Compatibility::OFFSET_MAP_SUPPORT) {
      if (header_.flags & STORE_Header::OFFSET_WITHIN_EPOCH &&
          header_.flags & STORE_Header::OFFSET_MAP) {
        // Size of offsetmap containing one counter
        rv += "01";
        // BYTE_OFFSET start index in hex. refer to OffsetMap.h
        rv += "F6";
      }
    }

    // Copyset, optional blobs, payload
    rv += extra_serialized_;

    // StoreChainLink
    rv += "010000000400008002000000050000800300000006000080";

    if (header_.flags & STORE_Header::CUSTOM_KEY) {
      // The protocol is:  1. Send number of keys. For here: "02".
      //                   2. For each key, send KeyType. Here, "00" or "01"
      //                   3. Length + serialized key (as before)
      if (optional_keys_.find(KeyType::FILTERABLE) != optional_keys_.end()) {
        rv += "0200"; //"02" means 2 optional keys, "00" type for first key
        rv += key_serialized_;
        rv += "01"; // "01" type for second key
      } else {
        rv += "0100"; // "01" means 1 optional key. "00" type for first key
      }
      rv += key_serialized_;
    }

    if (header_.flags & STORE_Header::E2E_TRACING_ON) {
      if (proto >= Compatibility::STORE_E2E_TRACING_SUPPORT) {
        rv += e2e_tracing_context_serialized_;
      }
    }

    rv += "6869"; // payload ("hi" in hex)
    return rv;
  }

 private:
  STORE_Header header_;
  std::vector<StoreChainLink> cs_;
  std::string payload_;
  STORE_Extra extra_;
  std::string extra_serialized_;
  std::map<KeyType, std::string> optional_keys_;
  std::string key_serialized_;
  std::string e2e_tracing_context_;
  std::string e2e_tracing_context_serialized_;
};
} // namespace

TEST_F(MessageSerializationTest, STORE) {
  STORE_Extra extra;
  extra.recovery_id = recovery_id_t(0x72555800c6fe911e);
  extra.recovery_epoch = epoch_t(0xc2f998f6);

  TestStoreMessageFactory factory;
  factory.setFlags(STORE_Header::RECOVERY);
  factory.setExtra(extra, "1E91FEC600585572F698F9C2");

  STORE_Message m = factory.message();
  auto check = [&](const STORE_Message& m2, uint16_t proto) {
    checkSTORE(m, m2, proto);
  };
  DO_TEST(m,
          check,
          Compatibility::MIN_PROTOCOL_SUPPORTED,
          Compatibility::MAX_PROTOCOL_SUPPORTED,
          std::bind(&TestStoreMessageFactory::serialized, &factory, arg::_1),
          [](ProtocolReader& r) { return STORE_Message::deserialize(r, 128); });
}

TEST_F(MessageSerializationTest, STORE_WithKey) {
  TestStoreMessageFactory factory;
  std::map<KeyType, std::string> optional_keys;
  optional_keys.insert(
      std::make_pair(KeyType::FINDKEY, std::string("abcdefgh")));
  factory.setFlags(STORE_Header::CUSTOM_KEY);
  factory.setKey(optional_keys, "08006162636465666768");
  STORE_Message m = factory.message();
  auto check = [&](const STORE_Message& m2, uint16_t proto) {
    checkSTORE(m, m2, proto);
  };
  DO_TEST(m,
          check,
          Compatibility::MIN_PROTOCOL_SUPPORTED,
          Compatibility::MAX_PROTOCOL_SUPPORTED,
          std::bind(&TestStoreMessageFactory::serialized, &factory, arg::_1),
          [](ProtocolReader& r) { return STORE_Message::deserialize(r, 128); });
}

TEST_F(MessageSerializationTest, STORE_WithFilterableKey) {
  TestStoreMessageFactory factory;
  std::map<KeyType, std::string> optional_keys;
  optional_keys.insert(
      std::make_pair(KeyType::FINDKEY, std::string("abcdefgh")));
  optional_keys.insert(
      std::make_pair(KeyType::FILTERABLE, std::string("abcdefgh")));
  factory.setFlags(STORE_Header::CUSTOM_KEY);
  factory.setKey(optional_keys, "08006162636465666768");
  STORE_Message m = factory.message();
  auto check = [&](const STORE_Message& m2, uint16_t proto) {
    checkSTORE(m, m2, proto);
  };
  DO_TEST(m,
          check,
          Compatibility::MIN_PROTOCOL_SUPPORTED,
          Compatibility::MAX_PROTOCOL_SUPPORTED,
          std::bind(&TestStoreMessageFactory::serialized, &factory, arg::_1),
          [](ProtocolReader& r) { return STORE_Message::deserialize(r, 128); });
}

TEST_F(MessageSerializationTest, STORE_WithRebuildingInfo2) {
  STORE_Extra extra;
  extra.rebuilding_version = 42;
  extra.rebuilding_wave = 3;
  extra.rebuilding_id = log_rebuilding_id_t(10);

  TestStoreMessageFactory factory;
  factory.setFlags(STORE_Header::REBUILDING);
  factory.setExtra(extra, "2A00000000000000030000000A00000000000000");

  STORE_Message m = factory.message();
  auto check = [&](const STORE_Message& m2, uint16_t proto) {
    checkSTORE(m, m2, proto);
  };
  DO_TEST(m,
          check,
          Compatibility::MIN_PROTOCOL_SUPPORTED,
          Compatibility::MAX_PROTOCOL_SUPPORTED,
          std::bind(&TestStoreMessageFactory::serialized, &factory, arg::_1),
          [](ProtocolReader& r) { return STORE_Message::deserialize(r, 128); });
}

TEST_F(MessageSerializationTest, STORE_WithByteOffsetInfo) {
  STORE_Extra extra;
  extra.offset_within_epoch = 0x99;

  TestStoreMessageFactory factory;
  factory.setFlags(STORE_Header::OFFSET_WITHIN_EPOCH);
  factory.setExtra(extra, "9900000000000000");

  STORE_Message m = factory.message();
  auto check = [&](const STORE_Message& m2, uint16_t proto) {
    checkSTORE(m, m2, proto);
  };
  DO_TEST(m,
          check,
          Compatibility::MIN_PROTOCOL_SUPPORTED,
          Compatibility::MAX_PROTOCOL_SUPPORTED,
          std::bind(&TestStoreMessageFactory::serialized, &factory, arg::_1),
          [](ProtocolReader& r) { return STORE_Message::deserialize(r, 128); });
}

TEST_F(MessageSerializationTest, STORE_WithByteOffsetMapInfo) {
  STORE_Extra extra;
  extra.offset_within_epoch = 0x99;
  extra.offsets_within_epoch.setCounter(CounterType::BYTE_OFFSET, 0x99);

  TestStoreMessageFactory factory;
  factory.setFlags(STORE_Header::OFFSET_WITHIN_EPOCH |
                   STORE_Header::OFFSET_MAP);
  factory.setExtra(extra, "9900000000000000");

  STORE_Message m = factory.message();
  auto check = [&](const STORE_Message& m2, uint16_t proto) {
    checkSTORE(m, m2, proto);
  };
  DO_TEST(m,
          check,
          Compatibility::OFFSET_MAP_SUPPORT,
          Compatibility::MAX_PROTOCOL_SUPPORTED,
          std::bind(&TestStoreMessageFactory::serialized, &factory, arg::_1),
          [](ProtocolReader& r) { return STORE_Message::deserialize(r, 128); });
}

TEST_F(MessageSerializationTest, STORE_WithFirstAmendableOffset) {
  STORE_Extra extra;
  extra.first_amendable_offset = 0x55;

  TestStoreMessageFactory factory;
  factory.setWave(2);
  factory.setExtra(extra, "55");

  STORE_Message m = factory.message();
  auto check = [&](const STORE_Message& m2, uint16_t proto) {
    checkSTORE(m, m2, proto);
  };
  DO_TEST(m,
          check,
          Compatibility::MIN_PROTOCOL_SUPPORTED,
          Compatibility::MAX_PROTOCOL_SUPPORTED,
          std::bind(&TestStoreMessageFactory::serialized, &factory, arg::_1),
          [](ProtocolReader& r) { return STORE_Message::deserialize(r, 128); });
}

TEST_F(MessageSerializationTest, STORE_WithE2ETracingContext) {
  TestStoreMessageFactory factory;

  factory.setFlags(STORE_Header::E2E_TRACING_ON);
  factory.setE2ETracingContext("abcdefgh",
                               "0800000000000000"
                               "6162636465666768");

  STORE_Message m = factory.message();
  auto check = [&](const STORE_Message& m2, uint16_t proto) {
    checkSTORE(m, m2, proto);
  };
  DO_TEST(m,
          check,
          Compatibility::MIN_PROTOCOL_SUPPORTED,
          Compatibility::MAX_PROTOCOL_SUPPORTED,
          std::bind(&TestStoreMessageFactory::serialized, &factory, arg::_1),
          [](ProtocolReader& r) { return STORE_Message::deserialize(r, 128); });
}

TEST_F(MessageSerializationTest, SHUTDOWN_WithServerInstanceId) {
  SHUTDOWN_Header h = {E::SHUTDOWN, ServerInstanceId(10)};

  SHUTDOWN_Message m(h);
  auto check = [&](const SHUTDOWN_Message& m2, uint16_t proto) {
    checkSHUTDOWN(m, m2, proto);
  };
  {
    std::string expected = "17000A00000000000000";
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            [&](uint16_t /*proto*/) { return expected; },
            nullptr);
  }
}

TEST_F(MessageSerializationTest, GET_EPOCH_RECOVERY_METADATA_RangeEpoch) {
  GET_EPOCH_RECOVERY_METADATA_Header h = {logid_t(0),
                                          epoch_t(10),
                                          epoch_t(1),
                                          0,
                                          shard_index_t(0),
                                          shard_index_t(0),
                                          epoch_t(10),
                                          request_id_t(10)};

  GET_EPOCH_RECOVERY_METADATA_Message msg(h);
  auto check = [&](const GET_EPOCH_RECOVERY_METADATA_Message& m2,
                   uint16_t proto) {
    checkGetEpochRecoveryMetadata(msg, m2, proto);
  };
  {
    std::string expected_old = "00000000000000000A00000001000000000000000000";
    std::string expected_new =
        "00000000000000000A000000010000000000000000000A0000000A00000000000000";
    DO_TEST(msg,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            [&](uint16_t proto) {
              return (proto >= Compatibility::GET_EPOCH_RECOVERY_RANGE_SUPPORT)
                  ? expected_new
                  : expected_old;
            },
            nullptr);
  }
}

TEST_F(MessageSerializationTest, GET_EPOCH_RECOVERY_METADATA_REPLY_RangeEpoch) {
  GET_EPOCH_RECOVERY_METADATA_REPLY_Header h = {logid_t(0),
                                                epoch_t(10),
                                                epoch_t(1),
                                                0,
                                                E::OK,
                                                shard_index_t(0),
                                                shard_index_t(0),
                                                epoch_t(10),
                                                1,
                                                request_id_t(10)};

  std::vector<epoch_t> epochs;
  std::vector<Status> status;
  std::vector<std::string> metadata;

  for (int i = 1; i <= 10; i++) {
    epochs.push_back(epoch_t(i));
    if (i <= 5) {
      status.push_back(E::EMPTY);
    } else if (i > 6) {
      status.push_back(E::NOTREADY);
    } else {
      status.push_back(E::OK);
      EpochRecoveryMetadata erm(epoch_t(7), esn_t(10), esn_t(10), 0, 0, 0);
      ld_check(erm.valid());
      Slice slice = erm.serialize();
      std::string md(reinterpret_cast<const char*>(slice.data), slice.size);
      metadata.push_back(md);
    }
  }

  GET_EPOCH_RECOVERY_METADATA_REPLY_Message msg(
      h, std::move(epochs), std::move(status), std::move(metadata));

  auto check = [&](const GET_EPOCH_RECOVERY_METADATA_REPLY_Message& m2,
                   uint16_t proto) {
    checkGetEpochRecoveryMetadataReply(msg, m2, proto);
  };
  {
    std::string expected =
        "00000000000000000A0000000100000000000000000000000A00000001000000000000"
        "000A000000000000000100000002000000030000000400000005000000060000000700"
        "000008000000090000000A0000006D006D006D006D006D00000032003200320032001E"
        "000000070000000A0000000A000000000000000000000000000000000000000000";
    DO_TEST(msg,
            check,
            Compatibility::GET_EPOCH_RECOVERY_RANGE_SUPPORT,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            [&](uint16_t /*unused*/) { return expected; },
            nullptr);
  }
}

TEST_F(MessageSerializationTest,
       GET_EPOCH_RECOVERY_METADATA_REPLY_NewSenderOldReceiver) {
  GET_EPOCH_RECOVERY_METADATA_REPLY_Header h = {logid_t(0),
                                                epoch_t(10),
                                                epoch_t(9),
                                                0,
                                                E::OK,
                                                shard_index_t(0),
                                                shard_index_t(0),
                                                epoch_t(9),
                                                0,
                                                request_id_t(10)};

  std::vector<epoch_t> epochs;
  std::vector<Status> status;
  std::vector<std::string> metadata;

  epochs.push_back(epoch_t(9));
  status.push_back(E::EMPTY);

  GET_EPOCH_RECOVERY_METADATA_REPLY_Message msg(
      h, std::move(epochs), std::move(status), std::move(metadata));

  auto check = [&](const GET_EPOCH_RECOVERY_METADATA_REPLY_Message& m2,
                   uint16_t proto) {
    ASSERT_EQ(msg.header_.log_id, m2.header_.log_id);
    ASSERT_EQ(msg.header_.purge_to, m2.header_.purge_to);
    ASSERT_EQ(msg.header_.start, m2.header_.start);
    ASSERT_EQ(msg.header_.flags, m2.header_.flags);
    ASSERT_EQ(E::EMPTY, m2.header_.status);
    ASSERT_EQ(msg.header_.shard, m2.header_.shard);
    ASSERT_EQ(msg.header_.purging_shard, m2.header_.purging_shard);
    ASSERT_EQ(msg.header_.start, m2.header_.end);
    ASSERT_EQ(REQUEST_ID_INVALID, m2.header_.id);
    ASSERT_EQ(
        msg.header_.num_non_empty_epochs, m2.header_.num_non_empty_epochs);
    ASSERT_EQ(msg.status_, m2.status_);
  };
  {
    std::string expected =
        "00000000000000000A0000000900000000006D000000000000000000";
    DO_TEST(msg,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::GET_EPOCH_RECOVERY_RANGE_SUPPORT - 1,
            [&](uint16_t) { return expected; },
            nullptr);
  }

  GET_EPOCH_RECOVERY_METADATA_REPLY_Header h2 = {logid_t(0),
                                                 epoch_t(10),
                                                 epoch_t(9),
                                                 0,
                                                 E::EMPTY,
                                                 shard_index_t(0),
                                                 shard_index_t(0),
                                                 epoch_t(9),
                                                 0,
                                                 request_id_t(10)};

  std::vector<epoch_t> epochs2;
  std::vector<Status> status2;
  GET_EPOCH_RECOVERY_METADATA_REPLY_Message msg2(
      h2, std::move(epochs2), std::move(status2), std::move(metadata));

  auto check2 = [&](const GET_EPOCH_RECOVERY_METADATA_REPLY_Message& m2,
                    uint16_t proto) {
    ASSERT_EQ(msg2.header_.log_id, m2.header_.log_id);
    ASSERT_EQ(msg2.header_.purge_to, m2.header_.purge_to);
    ASSERT_EQ(msg2.header_.start, m2.header_.start);
    ASSERT_EQ(msg2.header_.flags, m2.header_.flags);
    ASSERT_EQ(msg2.header_.status, m2.header_.status);
    ASSERT_EQ(msg2.header_.shard, m2.header_.shard);
    ASSERT_EQ(msg2.header_.purging_shard, m2.header_.purging_shard);
    ASSERT_EQ(msg2.header_.start, m2.header_.end);
    ASSERT_EQ(
        msg2.header_.num_non_empty_epochs, m2.header_.num_non_empty_epochs);
    ASSERT_EQ(REQUEST_ID_INVALID, m2.header_.id);
  };
  {
    std::string expected =
        "00000000000000000A0000000900000000006D000000000000000000";
    DO_TEST(msg2,
            check2,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::GET_EPOCH_RECOVERY_RANGE_SUPPORT - 1,
            [&](uint16_t) { return expected; },
            nullptr);
  }
}

TEST_F(MessageSerializationTest, APPEND) {
  APPEND_Header h = {request_id_t(0xb64a0f255e281e45),
                     logid_t(0x3c3b4fa7a1299851),
                     epoch_t(0xee396b50),
                     0xed5b3efc,
                     APPEND_Header::CHECKSUM_64BIT |
                         APPEND_Header::REACTIVATE_IF_PREEMPTED |
                         APPEND_Header::BUFFERED_WRITER_BLOB};
  auto deserializer = [](ProtocolReader& reader) {
    return APPEND_Message::deserialize(reader, 128);
  };
  auto expected_fn = [](uint16_t) { return std::string(); };
  AppendAttributes attrs;
  {
    APPEND_Message m(h, LSN_INVALID, attrs, PayloadHolder(strdup("hello"), 5));
    auto check = [&](const APPEND_Message& m2, uint16_t proto) {
      checkAPPEND(m, m2, proto);
    };
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            expected_fn,
            deserializer);
  }
  {
    h.flags |= APPEND_Header::CUSTOM_KEY;
    std::string key{"abcdefgh"};
    attrs.optional_keys[KeyType::FINDKEY] = std::move(key);
    APPEND_Message m(h, LSN_INVALID, attrs, PayloadHolder(strdup("hello"), 5));
    auto check = [&](const APPEND_Message& m2, uint16_t proto) {
      checkAPPEND(m, m2, proto);
    };
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            expected_fn,
            deserializer);
  }
  {
    h.flags |= APPEND_Header::CUSTOM_KEY;
    std::string key{"abcdefgh"};
    std::string key2{"12345678"};
    attrs.optional_keys[KeyType::FINDKEY] = std::move(key);
    attrs.optional_keys[KeyType::FILTERABLE] = std::move(key2);
    APPEND_Message m(h, LSN_INVALID, attrs, PayloadHolder(strdup("hello"), 5));
    auto check = [&](const APPEND_Message& m2, uint16_t proto) {
      checkAPPEND(m, m2, proto);
    };
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            expected_fn,
            deserializer);
  }
  {
    h.flags |= APPEND_Header::CUSTOM_COUNTERS;
    std::map<uint8_t, int64_t> counters;
    counters[1] = 1;
    counters[2] = 2;
    counters[3] = 3;
    attrs.counters.emplace(std::move(counters));
    APPEND_Message m(h, LSN_INVALID, attrs, PayloadHolder(strdup("hello"), 5));
    auto check = [&](const APPEND_Message& m2, uint16_t proto) {
      checkAPPEND(m, m2, proto);
    };
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            expected_fn,
            deserializer);
  }
  {
    h.flags |= APPEND_Header::CUSTOM_KEY;
    attrs.optional_keys.insert(
        std::make_pair(KeyType::FINDKEY, std::string("abcdefgh")));
    h.flags |= APPEND_Header::CUSTOM_COUNTERS;
    std::map<uint8_t, int64_t> counters;
    counters[1] = 1;
    counters[2] = 2;
    counters[3] = 3;
    attrs.counters.emplace(std::move(counters));
    APPEND_Message m(h, LSN_INVALID, attrs, PayloadHolder(strdup("hello"), 5));
    auto check = [&](const APPEND_Message& m2, uint16_t proto) {
      checkAPPEND(m, m2, proto);
    };
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            expected_fn,
            deserializer);
  }
  {
    h.flags |= APPEND_Header::E2E_TRACING_ON;
    APPEND_Message m(h,
                     LSN_INVALID,
                     attrs,
                     PayloadHolder(strdup("hello"), 5),
                     "TRACING_INFORMATION");
    auto check = [&](const APPEND_Message& m2, uint16_t proto) {
      checkAPPEND(m, m2, proto);
    };
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            expected_fn,
            deserializer);
  }
}

TEST_F(MessageSerializationTest, RECORD) {
  RECORD_Header h = {
      logid_t(0xb1ae6d3809c1cdad),
      read_stream_id_t(0xf8822b40e1a45f42),
      0xe7933997c8a866b0,
      0xda6c898046f65fe7,
      RECORD_Header::CHECKSUM_PARITY | RECORD_Header::INCLUDES_EXTRA_METADATA,
  };
  ExtraMetadata reb = {
      {
          esn_t(0x0b7430da),
          0xdb270ae5,
          2,
      },
      {ShardID(0x1d53, 0), ShardID(0x0287, 0)},
      BYTE_OFFSET_INVALID,
  };
  std::string payload = "preved";
  RECORD_Message m(h,
                   TrafficClass::REBUILD,
                   Payload(payload.data(), payload.size()).dup(),
                   std::make_unique<ExtraMetadata>(reb));
  auto check = [&](const RECORD_Message& m2, uint16_t proto) {
    checkRECORD(m, m2, proto);
  };
}

namespace {
TailRecord genTailRecord(bool include_payload) {
  TailRecordHeader::flags_t flags =
      (include_payload ? TailRecordHeader::HAS_PAYLOAD : 0);
  flags |= TailRecordHeader::OFFSET_WITHIN_EPOCH;
  void* payload_flat = malloc(20);
  memset(payload_flat, 0, 20);
  std::strncpy((char*)payload_flat, "Tail Record Test.", 20);
  return TailRecord(TailRecordHeader{logid_t(0xBBC18E8AA44783D3),
                                     compose_lsn(epoch_t(933), esn_t(3347)),
                                     1502502135,
                                     {2349045994592},
                                     flags,
                                     {}},
                    include_payload
                        ? std::make_shared<PayloadHolder>(payload_flat, 20)
                        : nullptr);
}
} // namespace

TEST_F(MessageSerializationTest, SEALED) {
  SEALED_Header h = {
      logid_t(0xBBC18E8AA44783D3),
      epoch_t(2823409157),
      E::OK,
      lsn_t(0x34DDEBFA27),
      3, // lng_list_size
      7, // shard
      2  // num_tail_records
  };

  std::vector<TailRecord> tails({genTailRecord(false), genTailRecord(true)});

  OffsetMap o1;
  o1.setCounter(CounterType::BYTE_OFFSET, 9);
  OffsetMap o2;
  o2.setCounter(CounterType::BYTE_OFFSET, 10);
  OffsetMap o3;
  o3.setCounter(CounterType::BYTE_OFFSET, 11);

  SEALED_Message m(h,
                   {3, 4, 5},
                   Seal(epoch_t(9), NodeID(2, 1)),
                   {6, 7, 8},
                   {o1, o2, o3},
                   {13, 14, 15},
                   tails);
  auto check = [&](const SEALED_Message& m2, uint16_t proto) {
    checkSEALED(m, m2, proto);
  };

  {
    std::string expected =
        "D38347A48A8EC1BB05CE49A8000027FAEBDD3400000003000000070003000000000000"
        "0004000000000000000500000000000000090000000100020009000000000000000A00"
        "0000000000000B00000000000000060000000000000007000000000000000800000000"
        "000000";
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::TAIL_RECORD_IN_SEALED - 1,
            [&](uint16_t /*proto*/) { return expected; },
            nullptr);
  }
  {
    std::string expected =
        "D38347A48A8EC1BB05CE49A8000027FAEBDD3400000003000000070002000000030000"
        "0000000000040000000000000005000000000000000900000001000200090000000000"
        "00000A000000000000000B000000000000000600000000000000070000000000000008"
        "000000000000000D000000000000000E000000000000000F00000000000000D38347A4"
        "8A8EC1BB130D0000A5030000F75C8E590000000060540DEE2202000000020000000000"
        "00D38347A48A8EC1BB130D0000A5030000F75C8E590000000060540DEE220200000302"
        "00000000000018000000140000005461696C205265636F726420546573742E000000";

    // this test involves contructing an evbuffer based payload holder and has
    // to be done on a worker thread
    auto test = [&] {
      DO_TEST(m,
              check,
              Compatibility::TAIL_RECORD_IN_SEALED,
              Compatibility::OFFSET_MAP_SUPPORT_IN_SEALED_MSG - 1,
              [&](uint16_t /*proto*/) { return expected; },
              nullptr);
      return 0;
    };

    auto processor = make_test_processor(create_default_settings<Settings>());
    run_on_worker(processor.get(), /*worker_id=*/0, test);
  }
  {
    std::string expected =
        "D38347A48A8EC1BB05CE49A8000027FAEBDD3400000003000000070002000000030000"
        "000000000004000000000000000500000000000000090000000100020001F609000000"
        "0000000001F60A0000000000000001F60B000000000000000600000000000000070000"
        "000000000008000000000000000D000000000000000E000000000000000F0000000000"
        "0000D38347A48A8EC1BB130D0000A5030000F75C8E590000000060540DEE2202000000"
        "02000000000000D38347A48A8EC1BB130D0000A5030000F75C8E590000000060540DEE"
        "22020000030200000000000018000000140000005461696C205265636F726420546573"
        "742E000000";

    // this test involves contructing an evbuffer based payload holder and has
    // to be done on a worker thread
    auto test = [&] {
      DO_TEST(m,
              check,
              Compatibility::OFFSET_MAP_SUPPORT_IN_SEALED_MSG,
              Compatibility::MAX_PROTOCOL_SUPPORTED,
              [&](uint16_t /*proto*/) { return expected; },
              nullptr);
      return 0;
    };

    auto processor = make_test_processor(create_default_settings<Settings>());
    run_on_worker(processor.get(), /*worker_id=*/0, test);
  }
}

TEST_F(MessageSerializationTest, STARTED) {
  STARTED_Header h = {logid_t(0xDCC49E8FF44783D3),
                      read_stream_id_t(0x8B49478D2C473B3A),
                      E::REBUILDING,
                      filter_version_t(0x8B49478D2C473B3A),
                      0x9B7D7B3FEC8486AA, // last released lsn
                      shard_index_t{0}};

  STARTED_Message m(h);
  auto check = [&](const STARTED_Message& m2, uint16_t proto) {
    checkSTARTED(m, m2, proto);
  };

  {
    std::string expected = "D38347F48F9EC4DC3A3B472C8D47498B2D003A3B472C8D47498"
                           "BAA8684EC3F7B7D9B0000";
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            [&](uint16_t /*proto*/) { return expected; },
            nullptr);
  }
}

/**
 * This test validates that the serialization of the filtered_out list in
 * START message works regardless of its size.
 * It tries serializing START messages with
 * - empty filtered out
 * - filtered_out of size 3
 * - filtered_out of size 128 (greater than COPYSET_SIZE_MAX but smaller
 *   than uint8_t::max())
 * - filtered_out of size 500
 * for protocol prior to SUPPORT_LARGE_FILTERED_OUT_LIST the size of the
 * vector should be set in the num_filtered_out member of the header.
 * for protocol > SUPPORT_LARGE_FILTERED_OUT_LIST, num_filtered_out should
 * be set to 0, and the vector and its size should be encoded after the header.
 * In the last case (size 500), the serialization should fail for protocols
 * older than SUPPORT_LARGER_FILTERED_OUT_LIST, and succeeds for newer ones.
 */
TEST_F(MessageSerializationTest, START_num_filtered_out) {
  START_Header h = {logid_t(0xDCC49E8FF44783D3),
                    read_stream_id_t(0x8B49478D2C473B3A),
                    lsn_t(5),
                    lsn_t(13),
                    lsn_t(8),
                    START_Header::SINGLE_COPY_DELIVERY,
                    0,
                    filter_version_t(0x8B49478D2C473B3A),
                    0,
                    0,
                    SCDCopysetReordering::NONE,
                    shard_index_t{0}};

  small_shardset_t filtered_out;
  auto check = [&](const START_Message& msg, uint16_t proto) {
    ASSERT_EQ(msg.filtered_out_, filtered_out);
  };

  auto expect = [&](uint16_t proto) {
    if (filtered_out.empty()) {
      // expect num_filtered_out to be 0 and no element in filtered_out
      if (proto < Compatibility::SUPPORT_LARGER_FILTERED_OUT_LIST) {
        return "D38347F48F9EC4DC3A3B472C8D47498B05000000000000000D0000000000000"
               "0080000000000000080000000" // flags = SINGLE_COPY_DELIVERY
               "00003A3B472C8D47498B00"    // num_filtered_out = 0
               "000000000000000000000000000000000000000000"; // attrs_
      } else {
        return "D38347F48F9EC4DC3A3B472C8D47498B05000000000000000D0000000000000"
               "0080000000000000080000000" // flags = SINGLE_COPY_DELIVERY
               "00003A3B472C8D47498B00"    // num_filtered_out = 0
               "000000000000000000000000"  // empty filtered_out
               "0000000000000000000000000000000000"; // attrs_
      }
    } else if (filtered_out.size() == 3) {
      // expect num_filtered_out to be 3 and 3 elements encoded in filtered_out
      if (proto < Compatibility::SUPPORT_LARGER_FILTERED_OUT_LIST) {
        return "D38347F48F9EC4DC3A3B472C8D47498B05000000000000000D0000000000000"
               "0080000000000000080000000" // flags = SINGLE_COPY_DELIVERY
               "00003A3B472C8D47498B03"    // num_filtered_out = 3
               "0000000000000000"          // N0:S0
               "01000000"                  // N1:S0
               "02000000"                  // N2:S0
               "0000000000000000000000000000000000"; // attrs_
      } else {
        return "D38347F48F9EC4DC3A3B472C8D47498B05000000000000000D0000000000000"
               "0080000000000000080000000" // flags = SINGLE_COPY_DELIVERY
               "00003A3B472C8D47498B00"    // num_filtered_out = 0
               "000000000C00000000000000"  // vector size = 3 * 4
               "00000000"                  // N0:S0
               "01000000"                  // N1:S0
               "02000000"                  // N2:S0
               "0000000000000000000000000000000000"; // attrs_
      }
    } else if (filtered_out.size() == COPYSET_SIZE_MAX + 1) {
      // expect num_filtered_out to be 128 and as many elements encoded
      if (proto < Compatibility::SUPPORT_LARGER_FILTERED_OUT_LIST) {
        return "D38347F48F9EC4DC3A3B472C8D47498B05000000000000000D0000000000000"
               "0080000000000000080000000" // flags = SINGLE_COPY_DELIVERY
               "00003A3B472C8D47498B80"    // num_filtered_out = 128
               "0000000000000000"          // N0:S0
               "01000000"                  // ...
               "020000000300000004000000050000000600000007000000080000000900000"
               "00A0000000B0000000C0000000D0000000E0000000F00000010000000110000"
               "001200000013000000140000001500000016000000170000001800000019000"
               "0001A0000001B0000001C0000001D0000001E0000001F000000200000002100"
               "000022000000230000002400000025000000260000002700000028000000290"
               "000002A0000002B0000002C0000002D0000002E0000002F0000003000000031"
               "000000320000003300000034000000350000003600000037000000380000003"
               "90000003A0000003B0000003C0000003D0000003E0000003F00000040000000"
               "410000004200000043000000440000004500000046000000470000004800000"
               "0490000004A0000004B0000004C0000004D0000004E0000004F000000500000"
               "005100000052000000530000005400000055000000560000005700000058000"
               "000590000005A0000005B0000005C0000005D0000005E0000005F0000006000"
               "000061000000620000006300000064000000650000006600000067000000680"
               "00000690000006A0000006B0000006C0000006D0000006E0000006F00000070"
               "000000710000007200000073000000740000007500000076000000770000007"
               "8000000790000007A0000007B0000007C0000007D0000007E0000007F000000"
               "0000000000000000000000000000000000"; // attrs_
      } else {
        return "D38347F48F9EC4DC3A3B472C8D47498B05000000000000000D0000000000000"
               "0080000000000000080000000" // flags = SINGLE_COPY_DELIVERY
               "00003A3B472C8D47498B00"    // num_filtered_out = 0
               "000000000002000000000000"  // vector size = 128 * 4 = 512
               "00000000"                  // N0:S0
               "01000000"                  // ...
               "020000000300000004000000050000000600000007000000080000000900000"
               "00A0000000B0000000C0000000D0000000E0000000F00000010000000110000"
               "001200000013000000140000001500000016000000170000001800000019000"
               "0001A0000001B0000001C0000001D0000001E0000001F000000200000002100"
               "000022000000230000002400000025000000260000002700000028000000290"
               "000002A0000002B0000002C0000002D0000002E0000002F0000003000000031"
               "000000320000003300000034000000350000003600000037000000380000003"
               "90000003A0000003B0000003C0000003D0000003E0000003F00000040000000"
               "410000004200000043000000440000004500000046000000470000004800000"
               "0490000004A0000004B0000004C0000004D0000004E0000004F000000500000"
               "005100000052000000530000005400000055000000560000005700000058000"
               "000590000005A0000005B0000005C0000005D0000005E0000005F0000006000"
               "000061000000620000006300000064000000650000006600000067000000680"
               "00000690000006A0000006B0000006C0000006D0000006E0000006F00000070"
               "000000710000007200000073000000740000007500000076000000770000007"
               "8000000790000007A0000007B0000007C0000007D0000007E0000007F000000"
               "0000000000000000000000000000000000"; // attrs_
      }
    } else if (filtered_out.size() == 500) {
      // size is larger than what num_filtered_out can accommodate, so expect
      // num_filtered_out to be 0, and the vector size to be encoded before the
      // vector contents.
      return "D38347F48F9EC4DC3A3B472C8D47498B05000000000000000D000000000000000"
             "80000000000000080000000"  // flags = SINGLE_COPY_DELIVERY
             "00003A3B472C8D47498B00"   // num_filtered_out = 0
             "00000000D007000000000000" // actual num filtered out size = 0x07d0
                                        // = 2000 = 500 * 4 = 500 *
                                        // sizeof(ShardID)
             "00000000"                 // N0:S0
             "01000000"                 // ...
             "02000000030000000400000005000000060000000700000008000000090000000"
             "A0000000B0000000C0000000D0000000E0000000F000000100000001100000012"
             "000000130000001400000015000000160000001700000018000000190000001A0"
             "000001B0000001C0000001D0000001E0000001F00000020000000210000002200"
             "0000230000002400000025000000260000002700000028000000290000002A000"
             "0002B0000002C0000002D0000002E0000002F0000003000000031000000320000"
             "00330000003400000035000000360000003700000038000000390000003A00000"
             "03B0000003C0000003D0000003E0000003F000000400000004100000042000000"
             "430000004400000045000000460000004700000048000000490000004A0000004"
             "B0000004C0000004D0000004E0000004F00000050000000510000005200000053"
             "0000005400000055000000560000005700000058000000590000005A0000005B0"
             "000005C0000005D0000005E0000005F0000006000000061000000620000006300"
             "00006400000065000000660000006700000068000000690000006A0000006B000"
             "0006C0000006D0000006E0000006F000000700000007100000072000000730000"
             "007400000075000000760000007700000078000000790000007A0000007B00000"
             "07C0000007D0000007E0000007F00000080000000810000008200000083000000"
             "8400000085000000860000008700000088000000890000008A0000008B0000008"
             "C0000008D0000008E0000008F0000009000000091000000920000009300000094"
             "00000095000000960000009700000098000000990000009A0000009B0000009C0"
             "000009D0000009E0000009F000000A0000000A1000000A2000000A3000000A400"
             "0000A5000000A6000000A7000000A8000000A9000000AA000000AB000000AC000"
             "000AD000000AE000000AF000000B0000000B1000000B2000000B3000000B40000"
             "00B5000000B6000000B7000000B8000000B9000000BA000000BB000000BC00000"
             "0BD000000BE000000BF000000C0000000C1000000C2000000C3000000C4000000"
             "C5000000C6000000C7000000C8000000C9000000CA000000CB000000CC000000C"
             "D000000CE000000CF000000D0000000D1000000D2000000D3000000D4000000D5"
             "000000D6000000D7000000D8000000D9000000DA000000DB000000DC000000DD0"
             "00000DE000000DF000000E0000000E1000000E2000000E3000000E4000000E500"
             "0000E6000000E7000000E8000000E9000000EA000000EB000000EC000000ED000"
             "000EE000000EF000000F0000000F1000000F2000000F3000000F4000000F50000"
             "00F6000000F7000000F8000000F9000000FA000000FB000000FC000000FD00000"
             "0FE000000FF000000000100000101000002010000030100000401000005010000"
             "060100000701000008010000090100000A0100000B0100000C0100000D0100000"
             "E0100000F01000010010000110100001201000013010000140100001501000016"
             "0100001701000018010000190100001A0100001B0100001C0100001D0100001E0"
             "100001F0100002001000021010000220100002301000024010000250100002601"
             "00002701000028010000290100002A0100002B0100002C0100002D0100002E010"
             "0002F010000300100003101000032010000330100003401000035010000360100"
             "003701000038010000390100003A0100003B0100003C0100003D0100003E01000"
             "03F01000040010000410100004201000043010000440100004501000046010000"
             "4701000048010000490100004A0100004B0100004C0100004D0100004E0100004"
             "F0100005001000051010000520100005301000054010000550100005601000057"
             "01000058010000590100005A0100005B0100005C0100005D0100005E0100005F0"
             "10000600100006101000062010000630100006401000065010000660100006701"
             "000068010000690100006A0100006B0100006C0100006D0100006E0100006F010"
             "00070010000710100007201000073010000740100007501000076010000770100"
             "0078010000790100007A0100007B0100007C0100007D0100007E0100007F01000"
             "08001000081010000820100008301000084010000850100008601000087010000"
             "88010000890100008A0100008B0100008C0100008D0100008E0100008F0100009"
             "00100009101000092010000930100009401000095010000960100009701000098"
             "010000990100009A0100009B0100009C0100009D0100009E0100009F010000A00"
             "10000A1010000A2010000A3010000A4010000A5010000A6010000A7010000A801"
             "0000A9010000AA010000AB010000AC010000AD010000AE010000AF010000B0010"
             "000B1010000B2010000B3010000B4010000B5010000B6010000B7010000B80100"
             "00B9010000BA010000BB010000BC010000BD010000BE010000BF010000C001000"
             "0C1010000C2010000C3010000C4010000C5010000C6010000C7010000C8010000"
             "C9010000CA010000CB010000CC010000CD010000CE010000CF010000D0010000D"
             "1010000D2010000D3010000D4010000D5010000D6010000D7010000D8010000D9"
             "010000DA010000DB010000DC010000DD010000DE010000DF010000E0010000E10"
             "10000E2010000E3010000E4010000E5010000E6010000E7010000E8010000E901"
             "0000EA010000EB010000EC010000ED010000EE010000EF010000F0010000F1010"
             "000F2010000"                         // N498:S0
             "F3010000"                            // N499:S0
             "0000000000000000000000000000000000"; // attrs_
    } else {
      return "wrong";
    }
  };

  {
    START_Message m(h, filtered_out);
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            expect,
            nullptr);
  }

  {
    filtered_out = {ShardID(0, 0), ShardID(1, 0), ShardID(2, 0)};
    START_Message m(h, filtered_out);
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            expect,
            nullptr);
  }

  {
    filtered_out.clear();
    for (int i = 0; i < COPYSET_SIZE_MAX + 1; i++) {
      filtered_out.push_back(ShardID(i, 0));
    }
    START_Message m(h, filtered_out);
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            expect,
            nullptr);
  }

  {
    filtered_out.clear();
    for (int i = 0; i < 500; i++) {
      filtered_out.push_back(ShardID(i, 0));
    }
    START_Message m(h, filtered_out);
    // Validates that the serialization fails with INVALID_PARAM
    // for older versions becasue the vector is too big
    DO_TEST(m,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::SUPPORT_LARGER_FILTERED_OUT_LIST - 1,
            expect,
            nullptr,
            E::INVALID_PARAM);
    // Validates that it works with protocol versions newer than
    // SUPPORT_LARGER_FILTERED_OUT_LIST
    DO_TEST(m,
            check,
            Compatibility::SUPPORT_LARGER_FILTERED_OUT_LIST,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            expect,
            nullptr);
  }
}

TEST_F(MessageSerializationTest, CLEAN) {
  CLEAN_Header h = {
      logid_t(0xBBC18E8AA44783D3),
      epoch_t(2823409157),
      recovery_id_t(0x8B49478D2C473B3A),
      0,
      epoch_t(0x8F9EC4DC),
      esn_t(0x84EC3F7),
      esn_t(0xD2C473B3),
      4,                  // num absent nodes
      0x9B7D7B3FEC8486AA, // epoch_end_offset
      0xABB4842249C95413, // epoch_size
      3                   // shard
  };

  StorageSet absent_nodes{
      ShardID(9, 0), ShardID(8, 0), ShardID(113, 0), ShardID(7, 0)};
  CLEAN_Message m(h, absent_nodes);
  auto check = [&](const CLEAN_Message& m2, uint16_t proto) {
    checkCLEAN(m, m2, proto);
  };

  absent_nodes =
      StorageSet{ShardID(9, 8), ShardID(8, 2), ShardID(113, 0), ShardID(7, 3)};
  CLEAN_Message m2(h, absent_nodes);
  {
    std::string expected = "D38347A48A8EC1BB05CE49A83A3B472C8D47498B0000DCC49E8"
                           "FF7C34E08B373C4D20400AA8684EC3F7B7D9B1354C9492284B4"
                           "AB030009000800080002007100000007000300";
    DO_TEST(m2,
            check,
            Compatibility::MIN_PROTOCOL_SUPPORTED,
            Compatibility::MAX_PROTOCOL_SUPPORTED,
            [&](uint16_t /*proto*/) { return expected; },
            nullptr);
  }
}

namespace {
template <typename MSG, MessageType Type>
std::unique_ptr<MSG> deserialize(struct evbuffer* evbuf, size_t size) {
  const auto proto = Compatibility::MAX_PROTOCOL_SUPPORTED;
  ProtocolReader reader(Type, evbuf, size, proto);
  std::unique_ptr<Message> msg = MSG::deserialize(reader).msg;
  return checked_downcast<std::unique_ptr<MSG>>(std::move(msg));
}
} // namespace

// Composes a HELLOv2 message with extra bytes at the end and verifies that
// a) the message itself is correctly deserialized (extra bytes are ignored)
// b) the next message in the evbuffer can be successfully deserialized
TEST_F(MessageSerializationTest, DrainExtraBytes) {
  struct evbuffer* evbuf = LD_EV(evbuffer_new)();
  SCOPE_EXIT {
    LD_EV(evbuffer_free)(evbuf);
  };

  const uint16_t proto = Compatibility::MAX_PROTOCOL_SUPPORTED;
  const std::string extra = "foo";

  HELLO_Message hello_msg{HELLO_Header{
      Compatibility::MIN_PROTOCOL_SUPPORTED,
      Compatibility::MAX_PROTOCOL_SUPPORTED,
      HELLO_flags_t(0),
      request_id_t(0),
  }};
  ProtocolWriter w1(hello_msg.type_, evbuf, proto);
  hello_msg.serialize(w1);
  size_t hello_size = w1.result();
  ASSERT_LT(0, hello_size);
  // write some extra bytes after the HELLO message

  ASSERT_EQ(0, LD_EV(evbuffer_add)(evbuf, extra.data(), extra.size()));
  hello_size += extra.size();

  // Now write a FixedSizeMessage followed by a couple of trailing bytes (this
  // simulates adding extra members to the message header).
  DELETE_Message delete_msg{DELETE_Header{
      RecordID(esn_t(1), epoch_t(1), logid_t(1)), 1, 0 /* shard */
  }};
  ProtocolWriter w2(delete_msg.type_, evbuf, proto);
  delete_msg.serialize(w2);
  size_t delete_size = w2.result();
  ASSERT_LT(0, delete_size);

  // Deserialize each message from the evbuffer and verify that they match.
  auto hello_deserialized =
      deserialize<HELLO_Message, MessageType::HELLO>(evbuf, hello_size);
  ASSERT_EQ(0,
            memcmp(&hello_msg.header_,
                   &hello_deserialized->header_,
                   sizeof(hello_msg.header_)));
  auto delete_deserialized =
      deserialize<DELETE_Message, MessageType::HELLO>(evbuf, delete_size);
  ASSERT_EQ(0,
            memcmp(&delete_msg.getHeader(),
                   &delete_deserialized->getHeader(),
                   sizeof(delete_msg.getHeader())));
}

}} // namespace facebook::logdevice
