/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <gtest/gtest.h>

#include "logdevice/common/test/SocketTest_fixtures.h"

using namespace facebook::logdevice;

namespace {

TEST_F(FlowGroupTest, DisableTest) {
  update.policy.setEnabled(true);
  flow_group.applyUpdate(update);

  auto send_msgs = [this](bool expect_success) {
    size_t msg_size = 10;
    for (Priority p = Priority::MAX; p < Priority::NUM_PRIORITIES;
         p = priorityBelow(p)) {
      auto msg = std::make_unique<VarLengthTestMessage>(
          Compatibility::MAX_PROTOCOL_SUPPORTED, msg_size);
      auto e = socket_->registerMessage(std::move(msg));
      ASSERT_NE(e, nullptr);
      ASSERT_EQ(msg, nullptr);
      if (expect_success) {
        // Messages unimpeeded by traffic shaping aren't interesting to us.
        // Just discard them so we don't consume memory.
        ASSERT_TRUE(drain(*e, p));
        socket_->discardEnvelope(*e);
      } else {
        ASSERT_FALSE(drain(*e, p));
        // Queue up for bandwidth.
        push(*e, p);
      }
    }
  };

  // At this point, we should have enough bandwidth to send a message at each
  // priority level.
  send_msgs(/*expect_success*/ true);

  // Reset all meters to 0. Attempts to drain should fail.
  resetMeter(0);
  send_msgs(/*expect_success*/ false);
  // Nothing should have been sent yet. We have to run the FlowGroup first.
  CHECK_SENDQ();

  // Running an enabled FlowGroup without available bandwidth does nothing.
  run();
  CHECK_SENDQ();

  // Applying a disabled policy should allow the messages to be released.
  update.policy.setEnabled(false);
  flow_group.applyUpdate(update);
  run();
  flushOutputEvBuffer();
  for (Priority p = Priority::MAX; p < Priority::NUM_PRIORITIES;
       p = priorityBelow(p)) {
    CHECK_ON_SENT(MessageType::GET_SEQ_STATE, E::OK);
  }
  // Nothing should be left to send.
  flow_group.empty();

  update.policy.setEnabled(true);
  flow_group.applyUpdate(update);
  resetMeter(-1);
  // Reset the meter to -1 so all priority levels have debt. Attempts to drain
  // should fail.
  send_msgs(/*expect_success*/ false);
  // Nothing should have been sent yet. We have to run the FlowGroup first.
  CHECK_SENDQ();

  // Running without bandwidth budget does nothing.
  run();
  CHECK_SENDQ();

  // Applying a disabled policy should allow the messages to be released.
  update.policy.setEnabled(false);
  flow_group.applyUpdate(update);
  run();
  flushOutputEvBuffer();
  for (Priority p = Priority::MAX; p < Priority::NUM_PRIORITIES;
       p = priorityBelow(p)) {
    CHECK_ON_SENT(MessageType::GET_SEQ_STATE, E::OK);
  }
  // Nothing should be left to send.
  flow_group.empty();
}

TEST_F(FlowGroupTest, FlowGroupRunLimits) {
  class SlowCallback : public BWAvailableCallback {
   public:
    explicit SlowCallback(std::chrono::microseconds delay) : delay_(delay) {}

    void operator()(FlowGroup&, std::mutex&) override {
      /* sleep override */
      std::this_thread::sleep_for(delay_);
    }

   private:
    std::chrono::microseconds delay_;
  };

  // Test 1
  update.policy.setEnabled(true);
  flow_group.applyUpdate(update);

  // Increase deadline to reduce the chance of false positives if this
  // test is run in parallel with other, CPU intensive, tests.
  settings_.flow_groups_run_yield_interval = std::chrono::milliseconds(100);

  // Running without any pending work should return false since there
  // is no additional work pending.
  EXPECT_FALSE(run());

  SlowCallback callback1(settings_.flow_groups_run_yield_interval * 2);
  SlowCallback callback2(settings_.flow_groups_run_yield_interval / 2);
  flow_group.push(callback1, Priority::MAX);
  flow_group.push(callback2, Priority::MAX);

  // The first run should terminate after the first callback is processed
  // due to its excessive runtime, and signal more work is required by
  // returning true.
  EXPECT_TRUE(run());
  EXPECT_FALSE(callback1.active());
  EXPECT_TRUE(callback2.active());

  // Running again should release the second callback and return false since
  // the run deadline was not exceeded.
  EXPECT_FALSE(run());
  EXPECT_FALSE(callback2.active());

  // Test Case 2
  // Push the callbacks again. Disable policy and make sure after disabling we
  // should still be able to drain all the callback

  update.policy.setEnabled(false);
  flow_group.applyUpdate(update);
  flow_group.push(callback1, Priority::MAX);
  flow_group.push(callback2, Priority::MAX);
  EXPECT_TRUE(run());
  EXPECT_FALSE(callback1.active());
  EXPECT_TRUE(callback2.active());
  // Running again should release the second callback and return false since
  // the run deadline was not exceeded.
  EXPECT_FALSE(run());
  EXPECT_FALSE(callback2.active());

  // Test Case 3
  // Transition from enabled to disabled when flow_group already has pending
  // callbacks. Same expectations as Test Case 2.
  flow_group.push(callback1, Priority::MAX);
  flow_group.push(callback2, Priority::MAX);
  update.policy.setEnabled(false);
  flow_group.applyUpdate(update);
  EXPECT_TRUE(run());
  EXPECT_FALSE(callback1.active());
  EXPECT_TRUE(callback2.active());
  // Running again should release the second callback and return false since
  // the run deadline was not exceeded.
  EXPECT_FALSE(run());
  EXPECT_FALSE(callback2.active());
}

TEST_F(FlowGroupTest, FlowGroupBandwidthCaps) {
  auto send_msg = [this](size_t msg_size, bool expect_success) {
    // Pretend to send a message of msg_size.
    auto msg = std::make_unique<VarLengthTestMessage>(
        Compatibility::MAX_PROTOCOL_SUPPORTED, msg_size, TrafficClass::REBUILD);
    auto e = socket_->registerMessage(std::move(msg));
    if (expect_success) {
      // Messages unimpeeded by traffic shaping aren't interesting to us.
      // Just discard them so we don't consume memory.
      ASSERT_TRUE(flow_group.drain(*e));
      socket_->discardEnvelope(*e);
    } else {
      ASSERT_FALSE(flow_group.drain(*e));
      // Queue up for bandwidth.
      flow_group.push(*e);
    }
  };

  // Limit the rebuild traffic to a max of 11KB.
  const int64_t BACKGROUND_BUCKET_SIZE = 10000;
  update.policy.set(Priority::BACKGROUND,
                    /*burst*/ BACKGROUND_BUCKET_SIZE,
                    /*guaranteed_bps*/ BACKGROUND_BUCKET_SIZE,
                    /*max_bps*/ BACKGROUND_BUCKET_SIZE + 1000);
  // Ensure the PQ bucket has plenty to give.
  update.policy.set(Priority::NUM_PRIORITIES,
                    /*burst*/ 3 * BACKGROUND_BUCKET_SIZE,
                    /*guaranteed_bps*/ 3 * BACKGROUND_BUCKET_SIZE,
                    /*max_bps*/ 3 * BACKGROUND_BUCKET_SIZE);
  resetMeter(0);
  flow_group.applyUpdate(update);
  ASSERT_EQ(flow_group.debt(Priority::BACKGROUND), 0);

  // At this point, we should have BACKGROUND_BUCKET_SIZE budget to use
  // directly, and an extra 1000 we can borrow from the priorityq.
  // (depositBudget has already been reduced by filling the bucket
  // with BACKGROUND_BUCKET_SIZE during applyUpdate()).
  ASSERT_EQ(flow_group.depositBudget(Priority::BACKGROUND), 1000);

  // Sending the first BACKGROUND_BUCKET_SIZE shouldn't incur any debt.
  for (size_t bytes_remaining = BACKGROUND_BUCKET_SIZE;
       bytes_remaining > sizeof(ProtocolHeader);) {
    auto msg_size = std::min((size_t)BACKGROUND_BUCKET_SIZE / 10,
                             bytes_remaining - sizeof(ProtocolHeader));
    send_msg(msg_size, /*expect_success*/ true);
    bytes_remaining -= msg_size + sizeof(ProtocolHeader);
    ASSERT_EQ(flow_group.debt(Priority::BACKGROUND), 0);
  }

  // The next message can only be sent once BW is transferred from the
  // priorityq bucket during a flow group run. Since the priorityq bucket
  // is able to pay for all of this message, our debt level should stay
  // at 0.
  send_msg(/*msg_size*/ 500, /*expect_success*/ false);
  ASSERT_EQ(flow_group.debt(Priority::BACKGROUND), 0);
  run();
  flushOutputEvBuffer();
  CHECK_ON_SENT(MessageType::GET_SEQ_STATE, E::OK);
  ASSERT_EQ(flow_group.debt(Priority::BACKGROUND), 0);

  // This next message will take us over our max. We allow it to be sent
  // using BW budget from the priorityq bucket, but we'll be left with a
  // debt since the max_bps limit has prevented the PQ bucket from
  // paying for the full message.
  send_msg(/*msg_size*/ 1000, /*expect_success*/ false);
  ASSERT_EQ(flow_group.debt(Priority::BACKGROUND), 0);
  run();
  flushOutputEvBuffer();
  CHECK_ON_SENT(MessageType::GET_SEQ_STATE, E::OK);
  ASSERT_TRUE(flow_group.debt(Priority::BACKGROUND) > 0);

  // A message queued now will not be sent, even after a flow group run.
  send_msg(/*msg_size*/ 50, /*expect_success*/ false);
  run();
  // Nothing sent.
  CHECK_SENDQ();

  // Another update should reset the max cap and allow the message to be
  // dispatched.
  flow_group.applyUpdate(update);
  run();
  flushOutputEvBuffer();
  CHECK_ON_SENT(MessageType::GET_SEQ_STATE, E::OK);
  ASSERT_EQ(flow_group.debt(Priority::BACKGROUND), 0);
}

TEST_F(FlowGroupTest, FlowMeterFill) {
  FlowMeter test_meter;
  auto& test_bucket = test_meter.entries[0];
  size_t bucket_capacity = 300;

  // With a deposit budget of 200, we should be able to fill up to
  // 200 bytes. This won't exceed the specified bucket capacity.
  test_bucket.resetDepositBudget(200);

  ASSERT_EQ(test_bucket.fill(25, bucket_capacity), 0);
  ASSERT_EQ(test_bucket.fill(25, bucket_capacity), 0);
  ASSERT_EQ(test_bucket.fill(25, bucket_capacity), 0);
  // We are limited by the deposit budget.
  ASSERT_EQ(test_bucket.fill(150, bucket_capacity), 25);
  ASSERT_EQ(test_bucket.level(), 200);
  // And continue to be until it is reset
  ASSERT_EQ(test_bucket.fill(25, bucket_capacity), 25);
  ASSERT_EQ(test_bucket.level(), 200);

  // If we give the meter more deposity budget, we can fill up to the
  // capacity limit.
  test_bucket.resetDepositBudget(200);
  ASSERT_EQ(test_bucket.fill(25, bucket_capacity), 0);
  ASSERT_EQ(test_bucket.fill(25, bucket_capacity), 0);
  ASSERT_EQ(test_bucket.fill(25, bucket_capacity), 0);
  // We are limited by capacity.
  ASSERT_EQ(test_bucket.fill(35, bucket_capacity), 10);
  ASSERT_EQ(test_bucket.level(), 300);

  // Still out of capacity. We can't fill.
  ASSERT_EQ(test_bucket.fill(25, bucket_capacity), 25);
  ASSERT_EQ(test_bucket.level(), 300);

  // But if we extend the capacity, we should again run until running
  // out of deposit budget.
  bucket_capacity = 500;
  ASSERT_EQ(test_bucket.fill(25, bucket_capacity), 0);
  ASSERT_EQ(test_bucket.fill(25, bucket_capacity), 0);
  ASSERT_EQ(test_bucket.fill(25, bucket_capacity), 0);
  // We are limited by the deposit budget.
  ASSERT_EQ(test_bucket.fill(40, bucket_capacity), 15);
  ASSERT_EQ(test_bucket.level(), 400);

  // If we shrink the bucket, the deposit budget is increased by
  // the amount we can no longer hold. This is a kind of retroactive
  // overflow.
  bucket_capacity = 100;
  ASSERT_EQ(test_bucket.fill(50, bucket_capacity), 350);
  ASSERT_EQ(test_bucket.level(), 100);
  bucket_capacity = 500;
  // Limited by deposit budget which is now 300.
  ASSERT_EQ(test_bucket.fill(350, bucket_capacity), 50);
  ASSERT_EQ(test_bucket.level(), 400);

  // Test whether dropping the bucket capacity with max deposit_budget should
  // not lead to wrap around of deposit_budget.
  test_bucket.resetDepositBudget(INT64_MAX);
  bucket_capacity = 300;
  ASSERT_EQ(test_bucket.fill(50, bucket_capacity), 150);
  ASSERT_EQ(test_bucket.level(), 300);
  ASSERT_EQ(test_bucket.depositBudget(), INT64_MAX);
}

TEST_F(FlowGroupTest, FlowMeterTransferCredit) {
  FlowMeter test_meter;
  auto& test_bucket = test_meter.entries[0];
  auto bucket_capacity = 100;
  // Give test bucket an explicit deposit budget.
  // All other buckets have effectively no limit(INT64_MAX).
  test_bucket.resetDepositBudget(50);
  // Filling or transferring credits into a bucket is limited by destination
  // bucket's deposit budget.
  auto& backup_bucket = test_meter.entries[1];
  ASSERT_EQ(backup_bucket.depositBudget(), INT64_MAX);
  ASSERT_EQ(backup_bucket.fill(100, bucket_capacity), 0);
  ASSERT_EQ(backup_bucket.level(), 100);
  backup_bucket.transferCredit(test_bucket, bucket_capacity);
  ASSERT_EQ(test_bucket.level(), 50);
  ASSERT_EQ(backup_bucket.level(), 50);

  // Transfer of credits will be limited by credits available in the source
  // bucket.
  test_bucket.resetDepositBudget(100);
  backup_bucket.transferCredit(test_bucket, bucket_capacity);
  ASSERT_EQ(backup_bucket.level(), 0);
  ASSERT_EQ(test_bucket.level(), 100);
}

TEST_F(FlowGroupTest, AllowCompleteBandwidthUsageForBurstyTraffic) {
  constexpr auto BURST_SIZE = 20;
  constexpr auto MSG_SIZE = 1000;
  constexpr auto TOTAL_MSG_SIZE = sizeof(ProtocolHeader) + MSG_SIZE;
  auto send_msg = [&]() {
    // Pretend to send a message of MSG_SIZE.
    auto msg = std::make_unique<VarLengthTestMessage>(
        Compatibility::MAX_PROTOCOL_SUPPORTED, MSG_SIZE, TrafficClass::RSM);
    auto e = socket_->registerMessage(std::move(msg));
    // No messages will be deferred by traffic shaping in this test, make sure
    // that is the case.
    ASSERT_TRUE(flow_group.drain(*e));
    // Just discard messages at this point so we don't consume memory.
    socket_->discardEnvelope(*e);
  };

  // Set the deposit budget (max bandwidth) to be total size of 10 messages. Set
  // burst capacity twice of deposit budget. The bucket config parameters for
  // traffic class MAX have following relationship:
  // burst capacity ( 20 * TOTAL_MSG_SIZE ) > max_bps (10 * TOTAL_MSG_SIZE) >
  // guaranteed_bps ( 5 * TOTAL_MSG_SIZE ).
  constexpr auto MAXP_BUCKET_SIZE = 10 * TOTAL_MSG_SIZE;
  update.policy.set(Priority::MAX,
                    /*burst*/ 2 * MAXP_BUCKET_SIZE,
                    /*guaranteed_bps*/ MAXP_BUCKET_SIZE / 2,
                    /*max_bps*/ MAXP_BUCKET_SIZE);
  // Ensure the PQ bucket has plenty to provide to traffic class MAX.
  update.policy.set(FlowGroup::PRIORITYQ_PRIORITY,
                    /*burst*/ MAXP_BUCKET_SIZE,
                    /*guaranteed_bps*/ MAXP_BUCKET_SIZE,
                    /*max_bps*/ MAXP_BUCKET_SIZE);

  // Rest of the classes are initialized. The values for their bucket config is
  // selected such that they don't borrow from priority queue traffic class.
  update.policy.set(Priority::CLIENT_HIGH,
                    /*burst*/ MAXP_BUCKET_SIZE,
                    /*guaranteed_bps*/ MAXP_BUCKET_SIZE,
                    /*max_bps*/ MAXP_BUCKET_SIZE);
  update.policy.set(Priority::CLIENT_NORMAL,
                    /*burst*/ MAXP_BUCKET_SIZE,
                    /*guaranteed_bps*/ MAXP_BUCKET_SIZE,
                    /*max_bps*/ MAXP_BUCKET_SIZE);
  update.policy.set(Priority::CLIENT_NORMAL,
                    /*burst*/ MAXP_BUCKET_SIZE,
                    /*guaranteed_bps*/ MAXP_BUCKET_SIZE,
                    /*max_bps*/ MAXP_BUCKET_SIZE);
  update.policy.set(Priority::BACKGROUND,
                    /*burst*/ MAXP_BUCKET_SIZE,
                    /*guaranteed_bps*/ MAXP_BUCKET_SIZE,
                    /*max_bps*/ MAXP_BUCKET_SIZE);
  update.policy.set(Priority::IDLE,
                    /*burst*/ MAXP_BUCKET_SIZE,
                    /*guaranteed_bps*/ MAXP_BUCKET_SIZE,
                    /*max_bps*/ MAXP_BUCKET_SIZE);

  // Reset bucket levels for all traffic classes to zero.
  resetMeter(0);

  // Prefill the priority queue bucket so it has sufficient credit to fill the
  // MAX bucket to capacity, when applyUpdate() distributes priority queue
  // bandwidth to other buckets.
  flow_group.resetMeter(FlowGroup::PRIORITYQ_PRIORITY,
                        /*level*/ MAXP_BUCKET_SIZE / 2);

  flow_group.applyUpdate(update);
  ASSERT_EQ(flow_group.debt(Priority::MAX), 0);
  ASSERT_EQ(flow_group.depositBudget(Priority::MAX), 0);
  ASSERT_EQ(flow_group.level(Priority::MAX), MAXP_BUCKET_SIZE);

  // If in the current interval none of the credits in Priority MAX class got
  // used, in the next interval we should allow it to use credits accrued from
  // both the intervals. This is done only if the burst capacity allows us to do
  // so. Generally speaking, new credits will be deposited into the bucket by
  // the traffic shaper from guaranteed allocation and the pq class. If there
  // are unused credits from previous iteration, this will lead to accrual of
  // credits in the bucket. This deposit will be limited by deposit budget
  // and bucket capacity.
  // Thus, if a traffic class is not using the credits entirely
  // in an iteration then credits keep on adding up in it's bucket. This way
  // credits accumulated from multiple previous iteration can be used and
  // requested bandwidth can be achieved.
  flow_group.applyUpdate(update);
  ASSERT_EQ(flow_group.debt(Priority::MAX), 0);
  ASSERT_EQ(flow_group.depositBudget(Priority::MAX), 0);
  ASSERT_EQ(flow_group.level(Priority::MAX), 2 * MAXP_BUCKET_SIZE);

  // If we try to add some more we won't be able to, because the burst capacity
  // will limit the number of credits in the bucket.
  flow_group.applyUpdate(update);
  ASSERT_EQ(flow_group.debt(Priority::MAX), 0);
  ASSERT_EQ(flow_group.level(Priority::MAX), 2 * MAXP_BUCKET_SIZE);
  ASSERT_EQ(flow_group.depositBudget(Priority::MAX), MAXP_BUCKET_SIZE);

  // Now if a burst of 20 messages comes in, it should go through without any
  // debt thus maintaining the requested bandwidth.
  for (auto bytes_remaining = BURST_SIZE * TOTAL_MSG_SIZE; bytes_remaining > 0;
       bytes_remaining -= TOTAL_MSG_SIZE) {
    send_msg();
    ASSERT_EQ(flow_group.debt(Priority::MAX), 0);
  }
}

} // anonymous namespace
