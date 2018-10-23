/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/common/test/ZookeeperClientInMemory.h"

#include <condition_variable>
#include <memory>
#include <mutex>

#include <folly/futures/Future.h>
#include <gtest/gtest.h>

using namespace facebook::logdevice;
using namespace folly;

namespace {
const std::string kFoo{"/foo"};
const std::string kBar{"/bar"};

std::vector<SemiFuture<Unit>> fs;
Promise<Unit> freeFnPromise;
Promise<Unit> staticMemberFnPromise;

void initBasicTests() {
  fs.emplace_back(freeFnPromise.getSemiFuture());
  fs.emplace_back(staticMemberFnPromise.getSemiFuture());
}

void getDataCallback(int rc, StringPiece, zk::Stat) {
  EXPECT_EQ(ZNONODE, rc);
  freeFnPromise.setValue();
}

struct C {
  static void getDataCallback(int rc, StringPiece, zk::Stat) {
    EXPECT_EQ(ZNONODE, rc);
    staticMemberFnPromise.setValue();
  }
};

void runBasicTests(std::unique_ptr<ZookeeperClientInMemory> z) {
  initBasicTests();

  Promise<Unit> p0;
  fs.emplace_back(p0.getSemiFuture());

  // Test different types of callbacks (folly::Function, free function pointer,
  // member function pointer) can be handled correctly without leak
  auto cb = [p = std::move(p0)](int rc, StringPiece, zk::Stat) mutable {
    // znode does not exist yet
    EXPECT_EQ(ZNONODE, rc);
    p.setValue();
  };
  EXPECT_EQ(ZOK, z->getData(kBar, std::move(cb)));
  EXPECT_EQ(ZOK, z->getData(kBar, getDataCallback));
  EXPECT_EQ(ZOK, z->getData(kBar, C::getDataCallback));

  collectAll(std::move(fs)).wait();

  // write to the znode
  Promise<Unit> p1;
  auto f1 = p1.getSemiFuture();
  zk::version_t version = -1; // impossible value
  EXPECT_EQ(
      ZOK,
      z->setData(kFoo,
                 kBar,
                 [p = std::move(p1), &version](int rc, zk::Stat stat) mutable {
                   EXPECT_EQ(ZOK, rc);
                   version = stat.version_;
                   p.setValue();
                 }));
  std::move(f1).wait();
  EXPECT_GE(version, 0);

  // reads should now succeed
  Promise<Unit> p2;
  auto f2 = p2.getSemiFuture();
  EXPECT_EQ(ZOK,
            z->getData(kFoo,
                       [p = std::move(p2), version](
                           int rc, StringPiece value, zk::Stat stat) mutable {
                         EXPECT_EQ(ZOK, rc);
                         EXPECT_EQ(kBar, value);
                         EXPECT_EQ(version, stat.version_);
                         p.setValue();
                       }));
  std::move(f2).wait();

  // conditional writes
  Promise<Unit> p3;
  auto f3 = p3.getSemiFuture();

  EXPECT_EQ(ZOK,
            z->setData(kFoo,
                       "Should not update",
                       [p = std::move(p3)](int rc, zk::Stat stat) mutable {
                         EXPECT_EQ(ZBADVERSION, rc);
                         p.setValue();
                       },
                       version + 100));
  std::move(f3).wait();

  Promise<Unit> p4;
  auto f4 = p4.getSemiFuture();
  zk::version_t new_version = -1;
  EXPECT_EQ(ZOK,
            z->setData(kFoo,
                       "Conditional update",
                       [p = std::move(p4), &new_version](
                           int rc, zk::Stat stat) mutable {
                         EXPECT_EQ(ZOK, rc);
                         new_version = stat.version_;
                         p.setValue();
                       },
                       version));
  std::move(f4).wait();
  EXPECT_EQ(version + 1, new_version);

  Promise<Unit> p5;
  auto f5 = p5.getSemiFuture();
  EXPECT_EQ(ZOK,
            z->setData(kFoo,
                       "Bad version",
                       [p = std::move(p5), &new_version](
                           int rc, zk::Stat stat) mutable {
                         EXPECT_EQ(ZBADVERSION, rc);
                         new_version = stat.version_;
                         p.setValue();
                       },
                       version));
  std::move(f5).wait();

  Promise<Unit> p6;
  auto f6 = p6.getSemiFuture();
  EXPECT_EQ(ZOK,
            z->setData(kFoo,
                       "Blind write",
                       [p = std::move(p6)](int rc, zk::Stat) mutable {
                         EXPECT_EQ(ZOK, rc);
                         p.setValue();
                       },
                       /* version = */ -1));
  std::move(f6).wait();
}

void runMultiThreadedTests(std::unique_ptr<ZookeeperClientInMemory> z) {
  constexpr size_t kNumThreads = 10;
  constexpr size_t kIter = 1000;
  std::array<std::thread, kNumThreads> threads;
  std::atomic<uint64_t> successCnt{0};

  std::condition_variable cv;
  std::mutex m;
  bool start = false;

  // obtain initial version
  zk::version_t initialVersion = -1;
  {
    Promise<int> promise;
    auto fut = promise.getSemiFuture();
    z->getData(kFoo,
               [p = std::move(promise), &initialVersion](
                   int rc, folly::StringPiece, zk::Stat stat) mutable {
                 initialVersion = stat.version_;
                 p.setValue(rc);
               });
    ASSERT_EQ(ZOK, std::move(fut).get());
    ASSERT_GE(initialVersion, 0);
  }

  auto fn = [&, initialVersion]() {
    // wait to start
    std::unique_lock<std::mutex> lk{m};
    cv.wait(lk, [&]() { return start; });

    for (uint64_t k = 0; k < kIter; ++k) {
      zk::version_t base_version = initialVersion + k;
      Promise<zk::version_t> promise;
      auto f = promise.getSemiFuture();
      z->setData(kFoo,
                 folly::to<std::string>(k + 1),
                 [&successCnt, p = std::move(promise), base_version](
                     int rc, zk::Stat stat) mutable {
                   if (rc == ZOK) {
                     successCnt++;
                     EXPECT_GT(stat.version_, base_version);
                   } else {
                     EXPECT_EQ(ZBADVERSION, rc);
                   }
                   p.setValue(stat.version_);
                 },
                 base_version);
      // do not ignore
      static_cast<void>(std::move(f).get());
    }
  };
  for (auto i = 0; i < kNumThreads; ++i) {
    threads[i] = std::thread(fn);
  }

  {
    std::lock_guard<std::mutex> g{m};
    start = true;
  }
  cv.notify_all();
  for (auto& t : threads) {
    t.join();
  }

  std::string value;
  Promise<int> promise;
  auto fut = promise.getSemiFuture();
  z->getData(kFoo,
             [kIter, p = std::move(promise)](
                 int rc, StringPiece value, zk::Stat) mutable {
               EXPECT_EQ(kIter, folly::to<size_t>(value));
               p.setValue(rc);
             });

  EXPECT_EQ(ZOK, std::move(fut).get());
  EXPECT_EQ(kIter, successCnt.load());
}

} // namespace

TEST(ZookeeperClientInMemoryTest, basic) {
  runBasicTests(std::make_unique<ZookeeperClientInMemory>(
      "unused",
      ZookeeperClientInMemory::state_map_t{
          {kFoo, {"initValue", zk::Stat{.version_ = 4}}}}));
}

TEST(ZookeeperClientInMemoryTest, basicMT) {
  runMultiThreadedTests(std::make_unique<ZookeeperClientInMemory>(
      "unused",
      ZookeeperClientInMemory::state_map_t{
          {kFoo, {"initValue", zk::Stat{.version_ = 4}}}}));
}
