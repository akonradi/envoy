#include "common/event/scaled_range_timer.h"

#include "envoy/event/timer.h"
#include "common/event/dispatcher_impl.h"
#include "test/test_common/simulated_time_system.h"
#include "test/mocks/event/mocks.h"

#include "gtest/gtest.h"
#include <chrono>

namespace Envoy {
namespace Event {
namespace {

using testing::_;
using testing::AnyNumber;
using testing::ByMove;
using testing::DoAll;
using testing::InSequence;
using testing::InvokeArgument;
using testing::InvokeWithoutArgs;
using testing::Mock;
using testing::MockFunction;
using testing::NiceMock;
using testing::Return;
using testing::ReturnNew;
using testing::ReturnRef;
using testing::SaveArg;
using testing::StrictMock;

class ScaledRangeTimerManagerTest : public testing::Test {
public:
  ScaledRangeTimerManagerTest() {}

  SimulatedTimeSystem simulated_time;
  NiceMock<MockDispatcher> dispatcher_;
  MockTimer* manager_timer_;
};

TEST_F(ScaledRangeTimerManagerTest, CreateAndDestroy) {
  ScaledRangeTimerManager manager(dispatcher_, 1.0);
}

TEST_F(ScaledRangeTimerManagerTest, SingleTimerIsEnabled) {
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);
  ScaledRangeTimerManager manager(dispatcher_, 1.0);

  MockFunction<void()> cb;
  EXPECT_CALL(cb, Call());

  auto* timer_timer = new NiceMock<MockTimer>(&dispatcher_);
  auto timer = manager.createTimer(cb.AsStdFunction());
  EXPECT_FALSE(timer->enabled());

  timer->enableTimer(std::chrono::seconds(10), std::chrono::seconds(100));
  EXPECT_TRUE(timer->enabled());

  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(10));
  timer_timer->invokeCallback();
  EXPECT_TRUE(timer->enabled());

  // Put the ScaledRangeTimer in the 'triggerable' state but don't trigger it yet.
  manager.setScaleFactor(0);
  EXPECT_TRUE(timer->enabled());

  manager_timer_->invokeCallback();
  EXPECT_FALSE(timer->enabled());
}

TEST_F(ScaledRangeTimerManagerTest, DisableWhileDisabled) {
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);
  ScaledRangeTimerManager manager(dispatcher_, 1.0);

  StrictMock<MockFunction<void()>> cb;

  auto* timer_timer = new NiceMock<MockTimer>(&dispatcher_);
  auto timer = manager.createTimer(cb.AsStdFunction());
  EXPECT_FALSE(timer->enabled());
  EXPECT_FALSE(timer_timer->enabled());

  timer->disableTimer();
  EXPECT_FALSE(timer->enabled());
  EXPECT_FALSE(timer_timer->enabled());
}

TEST_F(ScaledRangeTimerManagerTest, DisableWhilePending) {
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);
  ScaledRangeTimerManager manager(dispatcher_, 1.0);

  StrictMock<MockFunction<void()>> cb;

  auto* timer_timer = new NiceMock<MockTimer>(&dispatcher_);
  auto timer = manager.createTimer(cb.AsStdFunction());
  timer->enableTimer(std::chrono::seconds(10), std::chrono::seconds(100));
  EXPECT_TRUE(timer->enabled());
  EXPECT_TRUE(timer_timer->enabled());

  timer->disableTimer();
  EXPECT_FALSE(timer->enabled());
  EXPECT_FALSE(timer_timer->enabled());
}

TEST_F(ScaledRangeTimerManagerTest, DisableWhileActive) {
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);
  ScaledRangeTimerManager manager(dispatcher_, 1.0);

  StrictMock<MockFunction<void()>> cb;

  auto* timer_timer = new NiceMock<MockTimer>(&dispatcher_);
  auto timer = manager.createTimer(cb.AsStdFunction());

  timer->enableTimer(std::chrono::seconds(10), std::chrono::seconds(100));
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(5));
  timer_timer->invokeCallback();
  EXPECT_TRUE(timer->enabled());
  EXPECT_FALSE(timer_timer->enabled());

  timer->disableTimer();
  EXPECT_FALSE(timer->enabled());
  EXPECT_FALSE(timer_timer->enabled());
}

TEST_F(ScaledRangeTimerManagerTest, DisableWhileTriggerable) {
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);
  ScaledRangeTimerManager manager(dispatcher_, 1.0);

  StrictMock<MockFunction<void()>> cb;

  auto* timer_timer = new NiceMock<MockTimer>(&dispatcher_);
  auto timer = manager.createTimer(cb.AsStdFunction());
  timer->enableTimer(std::chrono::seconds(10), std::chrono::seconds(100));
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(5));
  timer_timer->invokeCallback();
  manager.setScaleFactor(0);

  timer->disableTimer();
  EXPECT_FALSE(timer->enabled());
  EXPECT_FALSE(timer_timer->enabled());
}

TEST_F(ScaledRangeTimerManagerTest, ReRegisterOnCallback) {
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);
  ScaledRangeTimerManager manager(dispatcher_, 1.0);

  MockFunction<void()> cb;

  auto* timer_timer = new NiceMock<MockTimer>(&dispatcher_);
  auto timer = manager.createTimer(cb.AsStdFunction());

  EXPECT_CALL(cb, Call)
      .WillOnce([&timer] { timer->enableTimer(std::chrono::seconds(1), std::chrono::seconds(2)); })
      .WillOnce([] {});

  timer->enableTimer(std::chrono::seconds(1), std::chrono::seconds(2));
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  timer_timer->invokeCallback();
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  manager_timer_->invokeCallback();

  EXPECT_TRUE(timer->enabled());
  EXPECT_TRUE(timer_timer->enabled());

  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  timer_timer->invokeCallback();
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  manager_timer_->invokeCallback();

  EXPECT_FALSE(timer->enabled());
  EXPECT_FALSE(timer_timer->enabled());
}

TEST_F(ScaledRangeTimerManagerTest, SingleTimerTriggeredNoScaling) {
  MockFunction<void()> cb;

  auto* scaled_timer_pending_timer = new NiceMock<MockTimer>(&dispatcher_);
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);

  EXPECT_CALL(*scaled_timer_pending_timer, enableTimer(std::chrono::milliseconds(5000), _));
  EXPECT_CALL(*manager_timer_, enableTimer(std::chrono::milliseconds(4000), _));

  EXPECT_CALL(cb, Call());

  ScaledRangeTimerManager manager(dispatcher_, 1.0);

  auto scaled_timer = manager.createTimer(cb.AsStdFunction());
  scaled_timer->enableTimer(std::chrono::seconds(5), std::chrono::seconds(9));
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(5));
  scaled_timer_pending_timer->invokeCallback();

  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(4));
  manager_timer_->invokeCallback();
}

TEST_F(ScaledRangeTimerManagerTest, SingleTimerSameMinMax) {
  MockFunction<void()> cb;

  auto* scaled_timer_pending_timer = new NiceMock<MockTimer>(&dispatcher_);
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);

  EXPECT_CALL(*scaled_timer_pending_timer, enableTimer(std::chrono::milliseconds(1000), _));
  EXPECT_CALL(*manager_timer_, enableTimer(std::chrono::milliseconds(0), _));

  EXPECT_CALL(cb, Call());

  ScaledRangeTimerManager manager(dispatcher_, 1.0);

  auto timer = manager.createTimer(cb.AsStdFunction());

  timer->enableTimer(std::chrono::seconds(1), std::chrono::seconds(1));
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  scaled_timer_pending_timer->invokeCallback();
  manager_timer_->invokeCallback();
}

struct TimerGroup {
  TimerGroup(MockDispatcher& dispatcher, ScaledRangeTimerManager& manager)
      : callback(std::make_unique<MockFunction<void()>>()),
        timer(new NiceMock<MockTimer>(&dispatcher)),
        range_timer(manager.createTimer(callback->AsStdFunction())) {}
  std::unique_ptr<MockFunction<void()>> callback;
  MockTimer* timer;
  RangeTimerPtr range_timer;
};

TEST_F(ScaledRangeTimerManagerTest, MultipleTimersNoScaling) {
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);
  ScaledRangeTimerManager manager(dispatcher_, 1.0);
  std::vector<TimerGroup> timers;

  for (int i = 0; i < 3; ++i) {
    timers.emplace_back(dispatcher_, manager);
    EXPECT_CALL(*timers.rbegin()->callback, Call);
  }

  timers[0].range_timer->enableTimer(std::chrono::seconds(1), std::chrono::seconds(3));
  timers[1].range_timer->enableTimer(std::chrono::seconds(2), std::chrono::seconds(5));
  timers[2].range_timer->enableTimer(std::chrono::seconds(0), std::chrono::seconds(6));

  EXPECT_TRUE(manager_timer_->enabled());
  EXPECT_EQ(manager_timer_->deadline_, std::chrono::milliseconds(6 * 1000));
  EXPECT_THAT(timers[0].timer->enabled_, true);
  EXPECT_THAT(timers[1].timer->enabled_, true);
  EXPECT_THAT(timers[2].timer->enabled_, false);

  // Advance time by 1 second, so timers[0] hits its min.
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  timers[0].timer->invokeCallback();
  EXPECT_THAT(timers[0].timer->enabled_, false);
  EXPECT_TRUE(manager_timer_->enabled());
  // T = 1s; the minimum deadline is for timers[0] @ T = 3 seconds.
  EXPECT_EQ(manager_timer_->deadline_, std::chrono::milliseconds(2 * 1000));

  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  timers[1].timer->invokeCallback();
  EXPECT_THAT(timers[1].timer->enabled_, false);
  EXPECT_TRUE(manager_timer_->enabled());
  // T = 2s; the minimum deadline is for timers[0] @ T = 3 seconds
  EXPECT_EQ(manager_timer_->deadline_, std::chrono::milliseconds(1 * 1000));

  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  EXPECT_TRUE(manager_timer_->enabled());
  manager_timer_->invokeCallback();
  // T = 3s; the minimum deadline is for timers[0] @ T = 5 seconds
  EXPECT_EQ(manager_timer_->deadline_, std::chrono::milliseconds(2 * 1000));
  Mock::VerifyAndClearExpectations(timers[0].callback.get());

  // Advancing time in a big leap should be okay.
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(3));
  manager_timer_->invokeCallback();
}

TEST_F(ScaledRangeTimerManagerTest, MultipleTimersWithScaling) {
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);
  ScaledRangeTimerManager manager(dispatcher_, 1.0);
  std::vector<TimerGroup> timers;

  for (int i = 0; i < 3; ++i) {
    timers.emplace_back(dispatcher_, manager);
    EXPECT_CALL(*timers.rbegin()->callback, Call);
  }

  // timers[0] will fire between T = 1 and T = 3.
  timers[0].range_timer->enableTimer(std::chrono::seconds(1), std::chrono::seconds(3));
  manager.setScaleFactor(0.5);
  // Advance time to T = 1 second, so timers[0] hits its min.
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  timers[0].timer->invokeCallback();
  EXPECT_THAT(timers[0].timer->enabled_, false);

  // Enable timers[1] to fire between T = 2 and T = 6.
  timers[1].range_timer->enableTimer(std::chrono::seconds(1), std::chrono::seconds(5));
  // Enable timers[2] to fire between T = 6 and T = 10.
  timers[2].range_timer->enableTimer(std::chrono::seconds(5), std::chrono::seconds(9));

  // Advance time to T = 2, which should make timers[0] hit its scaled max.
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  manager_timer_->invokeCallback();
  Mock::VerifyAndClearExpectations(timers[0].callback.get());
  timers[1].timer->invokeCallback();

  // At 4x speed, timers[1] will fire in only 1 second.
  manager.setScaleFactor(0.25);
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  // Advance time to T = 3, which should make timers[1] hit its scaled max.
  manager_timer_->invokeCallback();
  Mock::VerifyAndClearExpectations(timers[1].callback.get());

  // Advance time to T = 6, which enables timers[2] to fire.
  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(3));
  timers[2].timer->invokeCallback();
  manager.setScaleFactor(0);
  // With a scale factor of 0, timers[2] should be ready to be fired immediately.
  EXPECT_THAT(manager_timer_->deadline_, std::chrono::milliseconds::zero());
  manager_timer_->invokeCallback();
}

TEST_F(ScaledRangeTimerManagerTest, MultipleTimersSameTimes) {
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);
  ScaledRangeTimerManager manager(dispatcher_, 1.0);
  std::vector<TimerGroup> timers;

  for (int i = 0; i < 3; ++i) {
    timers.emplace_back(dispatcher_, manager);
    EXPECT_CALL(*timers.rbegin()->callback, Call);
    timers[i].range_timer->enableTimer(std::chrono::seconds(1), std::chrono::seconds(2));
  }

  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  for (int i = 0; i < 3; ++i) {
    timers[i].timer->invokeCallback();
  }

  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  manager_timer_->invokeCallback();
}

TEST_F(ScaledRangeTimerManagerTest, TryToOverflowInternalCurrentTime) {
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);
  ScaledRangeTimerManager manager(dispatcher_, 1.0);
  std::vector<TimerGroup> timers;

  for (int i = 0; i < 3; ++i) {
    timers.emplace_back(dispatcher_, manager);
    EXPECT_CALL(*timers.rbegin()->callback, Call);
    timers[i].range_timer->enableTimer(std::chrono::seconds(10 * 1000),
                                       std::chrono::seconds(10 * 1000));
  }
  manager.setScaleFactor(std::numeric_limits<float>::min());

  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  for (int i = 0; i < 3; ++i) {
    timers[i].timer->invokeCallback();
  }

  dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  manager_timer_->invokeCallback();
}

TEST_F(ScaledRangeTimerManagerTest, ScheduleWithScalingFactorZero) {
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);
  ScaledRangeTimerManager manager(dispatcher_, 1.0);

  MockFunction<void()> cb;
  EXPECT_CALL(cb, Call);
  auto* timer_timer = new NiceMock<MockTimer>(&dispatcher_);
  auto timer = manager.createTimer(cb.AsStdFunction());
  manager.setScaleFactor(0);

  timer->enableTimer(std::chrono::seconds(0), std::chrono::seconds(1));
  EXPECT_FALSE(timer_timer->enabled());
  manager_timer_->invokeCallback();
}

TEST_F(ScaledRangeTimerManagerTest, ScaledTimesBecomeLarge) {
  manager_timer_ = new NiceMock<MockTimer>(&dispatcher_);
  ScaledRangeTimerManager manager(dispatcher_, 1.0);
  std::vector<TimerGroup> timers;

  for (int i = 0; i < 5; ++i) {
    timers.emplace_back(dispatcher_, manager);
    EXPECT_CALL(*timers.rbegin()->callback, Call);
  }

  // Set the scale factor so that the internal time will get close to the 64-bit max but won't be in
  // danger of overflowing.
  manager.setScaleFactor(0x1p-32);

  for (int i = 0; i < 5; ++i) {
    timers[i].range_timer->enableTimer(std::chrono::seconds(0), std::chrono::seconds(4L * 1L << 32));
    dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
  }
  for (int i = 0; i < 10; i++) {
    if (manager_timer_->enabled_) {
      dispatcher_.time_system_.timeSystem().advanceTimeWait(std::chrono::seconds(1));
      manager_timer_->invokeCallback();
    }
  }
}

} // namespace
} // namespace Event
} // namespace Envoy