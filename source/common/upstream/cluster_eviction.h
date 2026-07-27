#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "envoy/event/dispatcher.h"

#include "source/common/common/logger.h"

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Upstream {

// Removes clusters that the cluster manager has been asked to evict, once a caller-supplied policy
// says they are eligible. This is the generic machinery behind
// ClusterManager::setClusterEvictionPolicy.
//
// Each registered cluster carries a should_evict predicate that the manager evaluates, on the main
// thread, on a periodic sweep. When it returns true the injected callback is invoked.
//
// A single sweep timer, armed at the shortest registered check interval, evaluates every tracked
// cluster, so the cost is one mark-sweep pass instead of one timer per cluster. The check interval
// is an upper bound on evaluation latency: a cluster may be polled more often (whenever another
// cluster's shorter interval drives the sweep), never less.
//
// All methods must be called on the main thread, where cluster add/remove is safe.
class ClusterEvictionManager : Logger::Loggable<Logger::Id::upstream> {
public:
  // Returns true when the cluster should be evicted now. Evaluated on the main thread.
  using ShouldEvictCb = std::function<bool(absl::string_view)>;
  // Invoked on the main thread just after a cluster is removed for eviction.
  using OnEvictedCb = std::function<void(absl::string_view)>;
  // Removes a cluster that has been reclaimed. Invoked on the main thread.
  using RemoveClusterCb = std::function<void(absl::string_view)>;

  ClusterEvictionManager(Event::Dispatcher& main_thread_dispatcher, RemoveClusterCb remove_cluster);

  // Begins, or refreshes, an eviction policy for a cluster. check_interval bounds how long the
  // manager may wait before evaluating should_evict. A non-positive check_interval clears any
  // existing policy for the cluster (it is dropped from tracking if present). should_evict must be
  // set; on_evicted may be null.
  void setPolicy(absl::string_view cluster_name, std::chrono::milliseconds check_interval,
                 ShouldEvictCb should_evict, OnEvictedCb on_evicted);

  // Drops a cluster from tracking, e.g. because it was removed for a reason other than eviction.
  // Safe to call for an untracked cluster.
  void clearPolicy(absl::string_view cluster_name);

private:
  struct Policy {
    std::chrono::milliseconds check_interval{};
    ShouldEvictCb should_evict;
    OnEvictedCb on_evicted;
  };

  // Periodic eviction check over all tracked clusters.
  void sweep();

  // Arms the sweep timer (when clusters are tracked and it is not already running) at a cadence
  // equal to the shortest tracked check interval.
  void armSweepTimer();

  // Invalidates the cached sweep cadence so armSweepTimer() recomputes it from tracked_.
  void invalidateCadence() { min_check_interval_ = std::chrono::milliseconds::min(); }

  RemoveClusterCb remove_cluster_;
  const Event::TimerPtr sweep_timer_;
  absl::flat_hash_map<std::string, Policy> tracked_;
  // Lazily-computed minimum of tracked_ check intervals, or negative if invalidated.
  std::chrono::milliseconds min_check_interval_{std::chrono::milliseconds::min()};
};

using ClusterEvictionManagerPtr = std::unique_ptr<ClusterEvictionManager>;

} // namespace Upstream
} // namespace Envoy
