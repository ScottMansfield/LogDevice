/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logdevice/common/configuration/NodesConfigStore.h"

#include <condition_variable>
#include <mutex>
#include <ostream>

#include <folly/Conv.h>
#include <folly/json.h>
#include <folly/synchronization/Baton.h>
#include <gtest/gtest.h>

#include "logdevice/common/configuration/ZookeeperNodesConfigStore.h"
#include "logdevice/common/test/InMemNodesConfigStore.h"
#include "logdevice/common/test/ZookeeperClientInMemory.h"

using namespace facebook::logdevice;
using namespace facebook::logdevice::configuration;
using namespace facebook::logdevice::membership;

using version_t = NodesConfigStore::version_t;

namespace {
const std::string kFoo{"/foo"};
const std::string kBar{"/bar"};
std::unique_ptr<NodesConfigStore> store{nullptr};

const std::string kValue{"value"};
const std::string kVersion{"version"};

struct TestEntry {
  explicit TestEntry(version_t version, std::string value)
      : version_(version), value_(std::move(value)) {}

  explicit TestEntry(version_t::raw_type version, std::string value)
      : version_(version), value_(std::move(value)) {}

  static TestEntry fromSerialized(folly::StringPiece buf) {
    auto d = folly::parseJson(buf);
    version_t version{
        folly::to<typename version_t::raw_type>(d.at(kVersion).getString())};
    return TestEntry{version, d.at(kValue).getString()};
  }

  std::string serialize() const {
    folly::dynamic d = folly::dynamic::object(kValue, value_)(
        kVersion, folly::to<std::string>(version_.val()));
    return folly::toJson(d);
  }

  version_t version() const {
    return version_;
  }

  std::string value() const {
    return value_;
  }

  friend std::ostream& operator<<(std::ostream& os, const TestEntry& entry) {
    return os << entry.serialize(); // whatever needed to print bar to os
  }

 private:
  version_t version_;
  std::string value_;

  friend bool operator==(const TestEntry& lhs, const TestEntry& rhs);
};

bool operator==(const TestEntry& lhs, const TestEntry& rhs) {
  return lhs.version_ == rhs.version_ && lhs.value_ == rhs.value_;
}

folly::Optional<version_t> extractVersionFn(folly::StringPiece buf) {
  try {
    return TestEntry::fromSerialized(buf).version();
  } catch (const std::runtime_error&) {
    return folly::none;
  }
}

void checkAndResetBaton(folly::Baton<>& b) {
  using namespace std::chrono_literals;
  // The baton should have be "posted" way sooner. We do this so that if the
  // baton is not posted, and the program hangs, we get a clear timeout message
  // instead of waiting for the test to time out.
  EXPECT_TRUE(b.try_wait_for(1s));
  b.reset();
}

void runBasicTests(std::unique_ptr<NodesConfigStore> store,
                   bool initialWrite = true) {
  folly::Baton<> b;
  std::string value_out{};

  if (initialWrite) {
    // no config stored yet
    EXPECT_EQ(0, store->getConfig(kFoo, [&b](Status status, std::string) {
      EXPECT_EQ(Status::NOTFOUND, status);
      b.post();
    }));
    checkAndResetBaton(b);

    EXPECT_EQ(Status::NOTFOUND, store->getConfigSync(kFoo, &value_out));

    // initial write
    EXPECT_EQ(Status::OK,
              store->updateConfigSync(
                  kFoo, TestEntry{10, "foo123"}.serialize(), folly::none));

    EXPECT_EQ(Status::OK, store->getConfigSync(kFoo, &value_out));
    EXPECT_EQ(TestEntry(10, "foo123"), TestEntry::fromSerialized(value_out));
  }

  EXPECT_EQ(Status::NOTFOUND, store->getConfigSync(kBar, &value_out));

  // update: blind overwrite
  EXPECT_EQ(Status::OK,
            store->updateConfigSync(
                kFoo, TestEntry{12, "foo456"}.serialize(), folly::none));
  EXPECT_EQ(Status::OK, store->getConfigSync(kFoo, &value_out));
  auto e = TestEntry::fromSerialized(value_out);
  EXPECT_EQ(TestEntry(12, "foo456"), e);

  // update: conditional update
  MembershipVersion::Type prev_version{e.version().val() - 1};
  MembershipVersion::Type curr_version{e.version().val()};
  MembershipVersion::Type next_version{e.version().val() + 1};
  version_t version_out = MembershipVersion::EMPTY_VERSION;
  value_out = "";
  EXPECT_EQ(
      Status::VERSION_MISMATCH,
      store->updateConfigSync(kFoo,
                              TestEntry{next_version, "foo789"}.serialize(),
                              prev_version,
                              &version_out,
                              &value_out));
  EXPECT_EQ(curr_version, version_out);
  EXPECT_EQ(TestEntry(12, "foo456"), TestEntry::fromSerialized(value_out));
  EXPECT_EQ(0,
            store->updateConfig(
                kFoo,
                TestEntry{next_version, "foo789"}.serialize(),
                next_version,
                [&b, curr_version](
                    Status status, version_t version, std::string value) {
                  EXPECT_EQ(Status::VERSION_MISMATCH, status);
                  EXPECT_EQ(curr_version, version);
                  EXPECT_EQ(TestEntry(12, "foo456"),
                            TestEntry::fromSerialized(value));
                  b.post();
                }));
  checkAndResetBaton(b);

  EXPECT_EQ(
      Status::OK,
      store->updateConfigSync(
          kFoo, TestEntry{next_version, "foo789"}.serialize(), curr_version));
  EXPECT_EQ(0,
            store->getConfig(
                kFoo, [&b, next_version](Status status, std::string value) {
                  EXPECT_EQ(Status::OK, status);
                  EXPECT_EQ(TestEntry(next_version, "foo789"),
                            TestEntry::fromSerialized(std::move(value)));
                  b.post();
                }));
  checkAndResetBaton(b);
}

void runMultiThreadedTests(std::unique_ptr<NodesConfigStore> store) {
  constexpr size_t kNumThreads = 5;
  constexpr size_t kIter = 30;
  std::array<std::thread, kNumThreads> threads;
  std::atomic<uint64_t> successCnt{0};

  std::condition_variable cv;
  std::mutex m;
  bool start = false;

  auto f = [&](int thread_idx) {
    {
      std::unique_lock<std::mutex> lk{m};
      cv.wait(lk, [&]() { return start; });
    }

    LOG(INFO) << folly::sformat("thread {} started...", thread_idx);
    folly::Baton<> b;
    for (uint64_t k = 0; k < kIter; ++k) {
      version_t base_version{k};
      version_t next_version{k + 1};
      int rv = store->updateConfig(
          kFoo,
          TestEntry{next_version, "foo" + folly::to<std::string>(k + 1)}
              .serialize(),
          base_version,
          [&b, &base_version, &successCnt](
              Status status, version_t new_version, std::string value) {
            if (status == Status::OK) {
              successCnt++;
            } else {
              EXPECT_EQ(Status::VERSION_MISMATCH, status);
              if (new_version != MembershipVersion::EMPTY_VERSION) {
                auto entry = TestEntry::fromSerialized(value);
                EXPECT_GT(entry.version(), base_version);
                EXPECT_GT(new_version, base_version);
              }
            }
            b.post();
          });
      if (rv == 0) {
        // Note: if the check fails here, the thread dies but the test would
        // keep hanging unfortunately.
        checkAndResetBaton(b);
      } else {
        b.reset();
      }
    }
  };
  for (auto i = 0; i < kNumThreads; ++i) {
    threads[i] = std::thread(std::bind(f, i));
  }

  // write version 0 (i.e., ~provision)
  ASSERT_EQ(Status::OK,
            store->updateConfigSync(
                kFoo, TestEntry{0, "foobar"}.serialize(), folly::none));

  {
    std::lock_guard<std::mutex> g{m};
    start = true;
  }
  cv.notify_all();
  for (auto& t : threads) {
    t.join();
  }

  std::string value_out;
  EXPECT_EQ(Status::OK, store->getConfigSync(kFoo, &value_out));
  EXPECT_EQ(TestEntry(kIter, "foo" + folly::to<std::string>(kIter)),
            TestEntry::fromSerialized(std::move(value_out)));
  EXPECT_EQ(kIter, successCnt.load());
}
} // namespace

TEST(NodesConfigStore, basic) {
  runBasicTests(std::make_unique<InMemNodesConfigStore>(extractVersionFn));
}

TEST(NodesConfigStore, basicMT) {
  runMultiThreadedTests(
      std::make_unique<InMemNodesConfigStore>(extractVersionFn));
}

TEST(NodesConfigStore, zk_basic) {
  auto z = std::make_shared<ZookeeperClientInMemory>(
      "unused",
      ZookeeperClientInMemory::state_map_t{
          {kFoo,
           {TestEntry{0, "initValue"}.serialize(), zk::Stat{.version_ = 4}}}});
  runBasicTests(std::make_unique<ZookeeperNodesConfigStore>(
                    extractVersionFn, std::move(z)),
                /* initialWrite = */ false);
}

TEST(NodesConfigStore, zk_basicMT) {
  auto z = std::make_shared<ZookeeperClientInMemory>(
      "unused",
      ZookeeperClientInMemory::state_map_t{
          {kFoo,
           {TestEntry{0, "initValue"}.serialize(), zk::Stat{.version_ = 4}}}});
  runMultiThreadedTests(std::make_unique<ZookeeperNodesConfigStore>(
      extractVersionFn, std::move(z)));
}
