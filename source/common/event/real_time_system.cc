#include "common/event/real_time_system.h"

#include <chrono>

#include "common/common/assert.h"
#include "common/event/timer_impl.h"

namespace Envoy {
namespace Event {
namespace {

class RealScheduler : public Scheduler {
public:
  RealScheduler(BaseScheduler& base_scheduler, Dispatcher& dispatcher)
      : base_scheduler_(base_scheduler), dispatcher_(dispatcher) {}
  TimerPtr createTimer(const TimerCb& cb) override { return base_scheduler_.createTimer(cb, dispatcher_); };

private:
  BaseScheduler& base_scheduler_;
  Dispatcher& dispatcher_;
};

} // namespace

SchedulerPtr RealTimeSystem::createScheduler(Scheduler& base_scheduler, CallbackScheduler&,
                                             Dispatcher& d) {
  return std::make_unique<RealScheduler>(base_scheduler, d);
}

} // namespace Event
} // namespace Envoy
