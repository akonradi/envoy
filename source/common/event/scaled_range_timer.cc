#include "common/event/scaled_range_timer.h"

#include <chrono>
#include <cmath>
#include <memory>

#include "envoy/event/range_timer.h"
#include "envoy/event/timer.h"

#include "common/common/assert.h"
#include "common/common/scope_tracker.h"

namespace Envoy {
namespace Event {

/**
 * Implementation of RangeTimer that can be scaled by the backing manager object.
 *
 * Instances of this class exist in one of 3 states:
 *  - disabled: not enabled
 *  - waiting-for-min: enabled, min timeout not elapsed
 *  - scaling-max: enabled, min timeout elapsed, max timeout not elapsed
 */
class ScaledRangeTimerManager::RangeTimerImpl final : public RangeTimer {
public:
  RangeTimerImpl(TimerCb callback, ScaledRangeTimerManager& manager)
      : manager_(manager), callback_(callback),
        pending_timer_(manager.dispatcher_.createTimer([this] { onPendingTimerComplete(); })) {}

  ~RangeTimerImpl() override { disableTimer(); }

  void disableTimer() override {
    struct Dispatch {
      Dispatch(RangeTimerImpl& timer) : timer(timer) {}
      RangeTimerImpl& timer;
      void operator()(const Inactive&) {}
      void operator()(const WaitingForMin&) { timer.pending_timer_->disableTimer(); }
      void operator()(const ScalingMax& active) {
        timer.manager_.disableActiveTimer(active.duration, active.bucket_position);
      }
    };
    absl::visit(Dispatch(*this), state_);
    state_.emplace<Inactive>();
  }

  void enableTimer(const std::chrono::milliseconds& min_ms, const std::chrono::milliseconds& max_ms,
                   const ScopeTrackedObject* scope) override {
    disableTimer();
    const MonotonicTime now = manager_.dispatcher_.timeSource().monotonicTime();
    const MonotonicTime latest_trigger = now + max_ms;
    scope_ = scope;
    ENVOY_LOG_MISC(trace, "enableTimer called on {} for ({}ms, {}ms)", static_cast<void*>(this),
                   min_ms.count(), max_ms.count());
    if (min_ms <= std::chrono::milliseconds::zero()) {
      auto it = manager_.add(*this, max_ms);
      state_.emplace<ScalingMax>(max_ms, it);
    } else {
      state_.emplace<WaitingForMin>(latest_trigger);
      pending_timer_->enableTimer(min_ms);
    }
  }

  bool enabled() override { return !absl::holds_alternative<Inactive>(state_); }

  void trigger() {
    ASSERT(absl::holds_alternative<ScalingMax>(state_));
    ENVOY_LOG_MISC(trace, "RangeTimerImpl triggered: {}", static_cast<void*>(this));
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
  friend class ScaledRangeTimerManager;

  struct Inactive {};

  struct WaitingForMin {
    WaitingForMin(MonotonicTime latest_trigger) : latest_trigger(latest_trigger) {}

    const MonotonicTime latest_trigger;
  };

  struct ScalingMax {
    ScalingMax(MonotonicTime::duration duration,
               ScaledRangeTimerManager::BucketEnabledList::iterator bucket_position)
        : duration(duration), bucket_position(std::move(bucket_position)) {}
    const MonotonicTime::duration duration;
    const ScaledRangeTimerManager::BucketEnabledList::iterator bucket_position;
  };

  void onPendingTimerComplete() {
    ENVOY_LOG_MISC(info, "pending complete for {}", static_cast<void*>(this));
    ASSERT(absl::holds_alternative<WaitingForMin>(state_));
    WaitingForMin& pending = absl::get<WaitingForMin>(state_);

    const MonotonicTime now = manager_.dispatcher_.timeSource().monotonicTime();
    auto it = manager_.add(*this, pending.latest_trigger - now);
    state_.emplace<ScalingMax>(pending.latest_trigger - now, it);
  }

  ScaledRangeTimerManager& manager_;
  const TimerCb callback_;
  const TimerPtr pending_timer_;

  absl::variant<Inactive, WaitingForMin, ScalingMax> state_;
  const ScopeTrackedObject* scope_;
};

ScaledRangeTimerManager::ScaledRangeTimerManager(Dispatcher& dispatcher, float scale_factor,
                                                 std::chrono::milliseconds minimum_duration)
    : dispatcher_(dispatcher), minimum_duration_(minimum_duration), scale_factor_(scale_factor) {}

RangeTimerPtr ScaledRangeTimerManager::createTimer(TimerCb callback) {
  return std::make_unique<RangeTimerImpl>(callback, *this);
}

void ScaledRangeTimerManager::setScaleFactor(float scale_factor) {
  scale_factor_ = DurationScaleFactor(scale_factor);
  for (auto& bucket : buckets_) {
    if (!bucket.scaled_timers.empty()) {
      bucket.updateTimer(*this);
    }
  }
}

MonotonicTime::duration
ScaledRangeTimerManager::getBucketedDuration(MonotonicTime::duration max_duration) {
  return MonotonicTime::duration(static_cast<MonotonicTime::duration::rep>(
      std::pow(kBucketScaleFactor,
               std::floor(std::log(max_duration.count()) / std::log(kBucketScaleFactor)))));
}

void ScaledRangeTimerManager::Bucket::updateTimer(ScaledRangeTimerManager& manager) {
  if (scaled_timers.empty()) {
    if (timer->enabled()) {
      timer->disableTimer();
    }
  } else {
    auto& entry = scaled_timers.front();
    timer->enableTimer(std::chrono::duration_cast<std::chrono::milliseconds>(
        manager.scale_factor_.value() *
        (entry.latest_trigger_time - manager.dispatcher_.timeSource().monotonicTime())));
  }
}

ScaledRangeTimerManager::DurationScaleFactor::DurationScaleFactor(float value)
    : value_(value < 0 ? 0 : value < 1 ? value : 1) {}

float ScaledRangeTimerManager::DurationScaleFactor::value() const { return value_; }

ScaledRangeTimerManager::BucketEnabledList::iterator
ScaledRangeTimerManager::add(RangeTimerImpl& timer, MonotonicTime::duration max_duration) {
  max_duration = getBucketedDuration(max_duration);
  Bucket& bucket = getOrCreateBucket(max_duration);

  bucket.scaled_timers.emplace_back(timer, max_duration + dispatcher_.timeSource().monotonicTime());
  bucket.updateTimer(*this);
  return --(bucket.scaled_timers.end());
}

void ScaledRangeTimerManager::disableActiveTimer(
    MonotonicTime::duration max_duration, const BucketEnabledList::iterator& bucket_position) {
  auto& bucket = getOrCreateBucket(max_duration);
  bucket.scaled_timers.erase(bucket_position);
  bucket.updateTimer(*this);
}

void ScaledRangeTimerManager::onBucketTimer(int bucket_index) {
  auto& bucket = buckets_[bucket_index];
  ASSERT(!bucket.scaled_timers.empty());

  auto it = bucket.scaled_timers.begin();
  auto* timer = &it->timer;
  bucket.scaled_timers.erase(it);

  bucket.updateTimer(*this);

  timer->trigger();
}

ScaledRangeTimerManager::Bucket&
ScaledRangeTimerManager::getOrCreateBucket(MonotonicTime::duration max_duration) {
  const size_t index =
      max_duration <= minimum_duration_
          ? 0
          : std::floor(std::log(max_duration.count()) / std::log(kBucketScaleFactor) -
                       std::log(minimum_duration_.count()) / std::log(kBucketScaleFactor));

  while (buckets_.size() <= index) {
    Bucket bucket;
    bucket.timer = dispatcher_.createTimer([this, i = buckets_.size()] { onBucketTimer(i); });
    buckets_.push_back(std::move(bucket));
  }

  return buckets_[index];
}

} // namespace Event
} // namespace Envoy