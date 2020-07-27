#include "common/event/scaled_range_timer.h"

#include <chrono>
#include <memory>

#include "envoy/event/timer.h"

#include "common/common/assert.h"
#include "common/common/scope_tracker.h"

namespace Envoy {
namespace Event {

/**
 * Implementation of RangeTimer that can be scaled by the backing manager object.
 *
 * Instances of this class exist in one of 4 states:
 *  - inactive: not enabled
 *  - pending: enabled, min timeout not elapsed
 *  - active: enabled, min timeout elapsed, max timeout not elapsed
 *  - triggerable: enabled, max timeout elapsed
 */
class ScaledRangeTimerManager::ScaledRangeTimer final : public RangeTimer {
public:
  ScaledRangeTimer(TimerCb callback, ScaledRangeTimerManager& manager)
      : manager_(manager), callback_(callback),
        pending_timer_(manager.dispatcher_.createTimer([this] { onPendingTimerComplete(); })) {}

  ~ScaledRangeTimer() override { disableTimer(); }

  void disableTimer() override {
    struct Dispatch {
      Dispatch(ScaledRangeTimer& timer) : timer(timer) {}
      ScaledRangeTimer& timer;
      void operator()(const Inactive&) {}
      void operator()(const Pending&) { timer.pending_timer_->disableTimer(); }
      void operator()(const Active& active) { timer.manager_.removeActive(active.position); }
      void operator()(const Triggerable&) { timer.manager_.cancelWaitingForTrigger(&timer); }
    };
    absl::visit(Dispatch(*this), state_);
    state_.emplace<Inactive>();
  }

  void enableTimer(const std::chrono::milliseconds& min, const std::chrono::milliseconds& max,
                   const ScopeTrackedObject* scope) override {
    const MonotonicTime now = manager_.dispatcher_.timeSource().monotonicTime();
    disableTimer();

    scope_ = scope;
    if (min > std::chrono::milliseconds::zero()) {
      state_.emplace<Pending>(now + max);
      pending_timer_->enableTimer(min);
    } else {
      auto it_or_none = manager_.add(*this, max);
      if (it_or_none.has_value()) {
        state_.emplace<Active>(*it_or_none);
      } else {
        state_.emplace<Triggerable>();
      }
    }
  }

  bool enabled() override { return !absl::holds_alternative<Inactive>(state_); }

  // The methods below are called by ScaledRangeTimerManager.
  ScaledRangeTimer* prepareToTrigger() {
    ASSERT(absl::holds_alternative<Active>(state_));
    state_.emplace<Triggerable>();
    return this;
  }

  void trigger() {
    ASSERT(absl::holds_alternative<Triggerable>(state_));
    state_.emplace<Inactive>();
    if (scope_ == nullptr) {
      callback_();
    } else {
      ScopeTrackerScopeState scope(scope_, manager_.dispatcher_);
      callback_();
    }
    scope_ = nullptr;
  }

private:
  struct Inactive {};
  struct Pending {
    Pending(MonotonicTime latest_trigger) : latest_trigger(latest_trigger) {}
    MonotonicTime latest_trigger;
  };
  struct Active {
    Active(ScaledRangeTimerManager::ActiveTimerIterator position) : position(position) {}
    ScaledRangeTimerManager::ActiveTimerIterator position;
  };
  struct Triggerable {};

  void onPendingTimerComplete() {
    ASSERT(absl::holds_alternative<Pending>(state_));
    const MonotonicTime now = manager_.dispatcher_.timeSource().monotonicTime();
    auto it_or_none = manager_.add(*this, std::chrono::duration_cast<std::chrono::milliseconds>(
                                              absl::get<Pending>(state_).latest_trigger - now));
    if (it_or_none.has_value()) {
      state_.emplace<Active>(*it_or_none);
    } else {
      state_.emplace<Triggerable>();
    }
  }

  ScaledRangeTimerManager& manager_;
  const TimerCb callback_;
  const TimerPtr pending_timer_;
  absl::variant<Inactive, Pending, Active, Triggerable> state_;
  const ScopeTrackedObject* scope_{};
};

ScaledRangeTimerManager::ScaledRangeTimerManager(Dispatcher& dispatcher, float scale_factor)
    : dispatcher_(dispatcher), timer_(dispatcher.createTimer([this] {
        updateScaledTime();
        triggerWaiting();
        updateTimer();
      })),
      scale_factor_(scale_factor), last_event_time_(dispatcher.timeSource().monotonicTime()),
      current_scaled_time_(ScaledTime::min()) {}

RangeTimerPtr ScaledRangeTimerManager::createTimer(TimerCb callback) {
  return std::make_unique<ScaledRangeTimer>(callback, *this);
}

void ScaledRangeTimerManager::setScaleFactor(float scale_factor) {
  updateScaledTime();
  scale_factor_ = scale_factor;
  if (scale_factor_.value() == 0) {
    for (auto& active : active_timers_) {
      triggerable_timers_.insert(active.timer.prepareToTrigger());
    }
    active_timers_.clear();
  }
  updateTimer();
}

ScaledRangeTimerManager::ScaledTime::Duration::rep
ScaledRangeTimerManager::ScaledTime::count() const {
  return value_.count();
}

#define SCALED_TIME_OPERATOR(OP)                                                                   \
  bool ScaledRangeTimerManager::ScaledTime::operator OP(const ScaledTime& other) const {           \
    return value_ OP other.value_;                                                                 \
  }
SCALED_TIME_OPERATOR(==);
SCALED_TIME_OPERATOR(!=);
SCALED_TIME_OPERATOR(<=);
SCALED_TIME_OPERATOR(<);
SCALED_TIME_OPERATOR(>);
SCALED_TIME_OPERATOR(>=);
#undef SCALED_TIME_OPERATOR

ScaledRangeTimerManager::ActiveTimer::ActiveTimer(ScaledTime trigger_time, ScaledRangeTimer& timer)
    : trigger_time(trigger_time), timer(timer) {}

bool ScaledRangeTimerManager::ActiveTimer::operator<(const ActiveTimer& other) const {
  if (trigger_time == other.trigger_time) {
    return static_cast<const void*>(this) < static_cast<const void*>(&other);
  }
  return trigger_time < other.trigger_time;
}

absl::optional<ScaledRangeTimerManager::ActiveTimerIterator>
ScaledRangeTimerManager::add(ScaledRangeTimer& timer, const std::chrono::milliseconds max_time) {
  updateScaledTime();
  absl::optional<ScaledRangeTimerManager::ActiveTimerIterator> it;

  if (scale_factor_.value() == 0) {
    triggerable_timers_.emplace(&timer);
    it = absl::nullopt;
  } else {
    ScaledTime::Duration additional_time(max_time);
    if (additional_time >= ScaledTime::Duration::max() / 2) {
      additional_time = ScaledTime::Duration::max() / 2;
    }
    ASSERT(current_scaled_time_ + additional_time < ScaledTime::max());
    it = active_timers_.emplace(current_scaled_time_ + max_time, timer).first;
  }
  updateTimer();
  return it;
}

void ScaledRangeTimerManager::removeActive(ActiveTimerIterator iterator) {
  bool is_first = active_timers_.begin() == iterator;
  active_timers_.erase(iterator);

  if (is_first || active_timers_.empty()) {
    updateTimer();
  }
}

void ScaledRangeTimerManager::cancelWaitingForTrigger(ScaledRangeTimer* timer) {
  triggerable_timers_.erase(timer);
}

ScaledRangeTimerManager::DurationScaleFactor::DurationScaleFactor(float value)
    : value_(value < 0 ? 0 : value < 1 ? value : 1) {}

float ScaledRangeTimerManager::DurationScaleFactor::value() const { return value_; }

void ScaledRangeTimerManager::updateScaledTime() {
  ASSERT(current_scaled_time_ < ScaledTime::zero());

  const MonotonicTime now = dispatcher_.timeSource().monotonicTime();

  float scale = scale_factor_.value();
  if (scale == 0) {
    ASSERT(active_timers_.empty());
    current_scaled_time_ = ScaledTime::min();
    last_event_time_ = now;
    return;
  }

  if (active_timers_.empty()) {
    current_scaled_time_ = ScaledTime::min();
    last_event_time_ = now;
    return;
  }

  auto time_since_advance = now - last_event_time_;
  double delta = time_since_advance.count() / scale;
  if (delta > static_cast<double>(ScaledTime::max().count()) ||
      ScaledTime::Duration(static_cast<ScaledTime::Duration::rep>(delta)) >=
          ScaledTime::Duration::max() / 2) {
    // If the delta is too large to represent as a ScaledTime, then just trigger all the timers
    // because they can't be scheduled for that far in the future anyway.
    for (auto& timer : active_timers_) {
      triggerable_timers_.insert(timer.timer.prepareToTrigger());
    }
    active_timers_.clear();
    current_scaled_time_ = ScaledTime::min();
    last_event_time_ = now;
    return;
  }

  const ScaledTime::Duration delta_duration(static_cast<ScaledTime::Duration::rep>(delta));
  // The invariant coming in is that current_scaled_time_ < 0. The check above guarantees that
  // current_scaled_time_ + delta_duration won't overflow.
  ASSERT(delta_duration < ScaledTime::Duration::max() / 2);
  current_scaled_time_ += delta_duration;

  if (current_scaled_time_ >= ScaledTime::zero()) {
    // Restore the invariant by shifting all the scheduled timers back by the same amount. This
    // preserves the ordering, which is why it is safe even though the const_cast looks dangerous.
    // Note that this is an O(n) operation, but should be amortized over many scaled timer
    // operations since it only happens near overflow.
    //
    // It's possible that the current scaled time is after the first trigger time, so make sure
    // to avoid underflow by not subtracting more than that.

    auto offset = current_scaled_time_ - ScaledTime::min();

    while (!active_timers_.empty()) {
      auto active = active_timers_.begin();
      if (active->trigger_time > current_scaled_time_) {
        break;
      }
      triggerable_timers_.insert(active->timer.prepareToTrigger());
      active_timers_.erase(active);
    }

    for (auto& active : active_timers_) {
      // The std::set API forces const access so you can't modify the values in a way that would
      // reorder them. Intentionally override the std::set API since all key values are shifting by
      // the same amount.
      const_cast<ActiveTimer&>(active).trigger_time -= offset;
    }

    current_scaled_time_ = ScaledTime::min();
    last_event_time_ = now;
  } else {
    last_event_time_ = now;

    while (!active_timers_.empty() &&
           active_timers_.begin()->trigger_time <= current_scaled_time_) {
      triggerable_timers_.insert(active_timers_.begin()->timer.prepareToTrigger());
      active_timers_.erase(active_timers_.begin());
    }
  }
}

void ScaledRangeTimerManager::triggerWaiting() {
  for (auto* timer : triggerable_timers_) {
    timer->trigger();
  }
  triggerable_timers_.clear();
}

void ScaledRangeTimerManager::updateTimer() {
  if (!triggerable_timers_.empty()) {
    timer_->enableTimer(std::chrono::milliseconds::zero());
  } else if (active_timers_.empty()) {
    timer_->disableTimer();
  } else {
    timer_->enableTimer(std::max(std::chrono::milliseconds::zero(),
                                 std::chrono::duration_cast<std::chrono::milliseconds>(
                                     (active_timers_.begin()->trigger_time - current_scaled_time_) *
                                     scale_factor_.value())));
  }
}

} // namespace Event
} // namespace Envoy
