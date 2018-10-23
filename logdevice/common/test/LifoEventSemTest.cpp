/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/common/LifoEventSem.h"

#include <semaphore.h>
#include <thread>

#include <folly/Benchmark.h>
#include <folly/Random.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GTest.h>
#include <folly/test/DeterministicSchedule.h>

using namespace folly;
using namespace folly::test;
using namespace facebook::logdevice;

typedef LifoEventSemImpl<DeterministicAtomic> DLifoEventSem;
typedef DeterministicSchedule DSched;

namespace facebook { namespace logdevice { namespace detail {

template <>
void EventFdBaton<DeterministicAtomic>::post() {
  DeterministicSchedule::beforeSharedAccess();
  EventFdBatonBase::post();
  DeterministicSchedule::afterSharedAccess();
}

template <>
void EventFdBaton<DeterministicAtomic>::wait() {
  bool done = false;
  while (!done) {
    DeterministicSchedule::beforeSharedAccess();
    done = consume();
    DeterministicSchedule::afterSharedAccess();
  }
}
}}} // namespace facebook::logdevice::detail

LIFOSEM_DECLARE_POOL(DeterministicAtomic, 100000)

static void wait(LifoEventSem& sem) {
  // wait is private in LifoEventSem, because it is slow, but it is still
  // useful to test the machinery
  static_cast<folly::detail::LifoSemBase<
      facebook::logdevice::detail::EventFdBaton<std::atomic>,
      std::atomic>*>(&sem)
      ->wait();
}

TEST(LifoEventSem, basic) {
  LifoEventSem sem;
  EXPECT_FALSE(sem.tryWait());
  sem.post();
  EXPECT_TRUE(sem.tryWait());
  sem.post();
  wait(sem);
}

TEST(LifoEventSem, multi) {
  LifoEventSem sem;

  const int opsPerThread = 10000;
  std::thread threads[10];
  std::atomic<int> blocks(0);

  for (auto& thr : threads) {
    thr = std::thread([&] {
      int b = 0;
      for (int i = 0; i < opsPerThread; ++i) {
        if (!sem.tryWait()) {
          wait(sem);
          ++b;
        }
        sem.post();
      }
      blocks += b;
    });
  }

  // start the flood
  sem.post();

  for (auto& thr : threads) {
    thr.join();
  }

  LOG(INFO) << opsPerThread * sizeof(threads) / sizeof(threads[0])
            << " post/wait pairs, " << blocks << " blocked";
}

TEST(LifoEventSem, async) {
  LifoEventSem sem;

  const size_t totalOps = 1000000;
  std::thread threads[40];
  std::atomic<size_t> received(0);

  for (auto& thr : threads) {
    thr = std::thread([&] {
      auto waiter = sem.beginAsyncWait();
      size_t n = 0;
      try {
        while (true) {
          auto ready = waiter->poll();
          EXPECT_TRUE(ready);
          waiter->process([&] { ++n; }, 10);
        }
      } catch (ShutdownSemError& x) {
        // expected
      }
      received += n;
    });
  }

  sem.post(totalOps);
  sem.shutdown();
  for (auto& thr : threads) {
    thr.join();
  }

  EXPECT_EQ(0, sem.valueGuess());
  EXPECT_EQ(totalOps, received.load());
}

static void workerPoolSim(int n,
                          int posters,
                          int waiters,
                          int waitBatchSize,
                          bool useProcessBatch) {
  LifoEventSem sem;

  std::vector<std::thread> waitThreads;
  std::vector<std::thread> postThreads;
  std::atomic<bool> go(false);
  std::atomic<size_t> received(0);

  BENCHMARK_SUSPEND {
    for (int t = 0; t < waiters; ++t) {
      waitThreads.emplace_back([&] {
        auto waiter = sem.beginAsyncWait();
        size_t localReceived = 0;
        try {
          while (true) {
            auto ready = waiter->poll();
            EXPECT_TRUE(ready);
            if (useProcessBatch) {
              waiter->processBatch(
                  [&](uint32_t d) { localReceived += d; }, waitBatchSize);
            } else {
              waiter->process([&] { ++localReceived; }, waitBatchSize);
            }
          }
        } catch (ShutdownSemError& x) {
          // expected
        }
        received += localReceived;
      });
    }

    for (int t = 0; t < posters; ++t) {
      postThreads.emplace_back([&] {
        int mine = n / posters + (t < (n % posters) ? 1 : 0);
        while (!go.load()) {
          std::this_thread::yield();
        }
        for (int i = 0; i < mine; ++i) {
          // batching on the wait side avoids calls to poll() or epoll()
          // and is therefore important, batching here just avoids some
          // compare-and-swaps and is much less of an effect
          sem.post();
        }
      });
    }
  }

  go.store(true);
  for (auto& thr : postThreads) {
    thr.join();
  }
  sem.shutdown();
  for (auto& thr : waitThreads) {
    thr.join();
  }

  EXPECT_EQ(n, received.load());
}

BENCHMARK_NAMED_PARAM(workerPoolSim, no_multi_1_to_1, 1, 1, 1, false)
BENCHMARK_NAMED_PARAM(workerPoolSim, multi2_1_to_1, 1, 1, 2, false)
BENCHMARK_NAMED_PARAM(workerPoolSim, multi4_1_to_1, 1, 1, 4, false)
BENCHMARK_NAMED_PARAM(workerPoolSim, multi8_1_to_1, 1, 1, 8, false)
BENCHMARK_NAMED_PARAM(workerPoolSim, batch2_1_to_1, 1, 1, 2, true)
BENCHMARK_NAMED_PARAM(workerPoolSim, batch4_1_to_1, 1, 1, 4, true)
BENCHMARK_NAMED_PARAM(workerPoolSim, batch8_1_to_1, 1, 1, 8, true)
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM(workerPoolSim, no_multi_32_to_32, 32, 32, 1, false)
BENCHMARK_NAMED_PARAM(workerPoolSim, multi2_32_to_32, 32, 32, 2, false)
BENCHMARK_NAMED_PARAM(workerPoolSim, multi4_32_to_32, 32, 32, 4, false)
BENCHMARK_NAMED_PARAM(workerPoolSim, multi8_32_to_32, 32, 32, 8, false)
BENCHMARK_NAMED_PARAM(workerPoolSim, batch2_32_to_32, 32, 32, 2, true)
BENCHMARK_NAMED_PARAM(workerPoolSim, batch4_32_to_32, 32, 32, 4, true)
BENCHMARK_NAMED_PARAM(workerPoolSim, batch8_32_to_32, 32, 32, 8, true)
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM(workerPoolSim, no_multi_32_to_320, 32, 320, 1, false)
BENCHMARK_NAMED_PARAM(workerPoolSim, multi2_32_to_320, 32, 320, 2, false)
BENCHMARK_NAMED_PARAM(workerPoolSim, multi4_32_to_320, 32, 320, 4, false)
BENCHMARK_NAMED_PARAM(workerPoolSim, multi8_32_to_320, 32, 320, 8, false)
BENCHMARK_NAMED_PARAM(workerPoolSim, batch2_32_to_320, 32, 320, 2, true)
BENCHMARK_NAMED_PARAM(workerPoolSim, batch4_32_to_320, 32, 320, 4, true)
BENCHMARK_NAMED_PARAM(workerPoolSim, batch8_32_to_320, 32, 320, 8, true)

#if 0
int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  int rv = RUN_ALL_TESTS();
  folly::runBenchmarksOnFlag();
  return rv;
}
#endif
