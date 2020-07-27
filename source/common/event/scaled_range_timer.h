#include <chrono>

#include "envoy/event/timer.h"

#include "absl/container/flat_hash_map.h"
#include "absl/types/optional.h"

namespace Envoy {
namespace Event {

class ScaledRangeTimerManager final {
public:
  ScaledRangeTimerManager(Dispatcher& dispatcher, float scale_factor);

  RangeTimerPtr createTimer(TimerCb callback);

  void setScaleFactor(float scale_factor);

protected:
  class ScaledRangeTimer;
  class ScaledTime {
  public:
    using Duration = MonotonicTime::duration;
    static constexpr ScaledTime max() { return Duration::max(); }
    static constexpr ScaledTime min() { return Duration::min(); }
    static constexpr ScaledTime zero() { return Duration::zero(); }

    constexpr ScaledTime(Duration now) : value_(now) {}
    Duration::rep count() const;
    ScaledTime& operator+=(Duration delta) {
      value_ += delta;
      return *this;
    }
    ScaledTime& operator-=(Duration delta) {
      value_ -= delta;
      return *this;
    }

    ScaledTime operator+(Duration delta) const { return ScaledTime(value_ + delta); }
    ScaledTime operator-(Duration delta) const { return ScaledTime(value_ - delta); }
    Duration operator-(ScaledTime other) const { return value_ - other.value_; }

    bool operator==(const ScaledTime& other) const;
    bool operator!=(const ScaledTime& other) const;
    bool operator<=(const ScaledTime& other) const;
    bool operator<(const ScaledTime& other) const;
    bool operator>(const ScaledTime& other) const;
    bool operator>=(const ScaledTime& other) const;

  private:
    Duration value_;
  };

  struct ActiveTimer {
    ActiveTimer(ScaledTime trigger_time, ScaledRangeTimer& timer);
    ScaledTime trigger_time;
    ScaledRangeTimer& timer;

    bool operator<(const ActiveTimer& other) const;
  };

  using ActiveTimerList = std::set<ActiveTimer>;
  using ActiveTimerIterator = ActiveTimerList::iterator;

  absl::optional<ActiveTimerIterator> add(ScaledRangeTimer& timer,
                                          std::chrono::milliseconds max_time);

  void removeActive(ActiveTimerIterator entry);
  void cancelWaitingForTrigger(ScaledRangeTimer* timer);

  Dispatcher& dispatcher_;

private:
  class DurationScaleFactor {
  public:
    DurationScaleFactor(float value);
    float value() const;

  private:
    float value_;
  };

  void updateScaledTime();
  void triggerWaiting();
  void updateTimer();

  const TimerPtr timer_;
  DurationScaleFactor scale_factor_;
  MonotonicTime last_event_time_;

  // The current scaled time. As an invariant, always stays below ScaledTime::zero().
  ScaledTime current_scaled_time_;
  std::set<ActiveTimer> active_timers_;
  std::unordered_set<ScaledRangeTimer*> triggerable_timers_;
};

} // namespace Event
} // namespace Envoy