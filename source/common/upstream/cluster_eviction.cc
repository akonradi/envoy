#include "source/common/upstream/cluster_eviction.h"

#include <algorithm>
#include <utility>

namespace Envoy {
namespace Upstream {

ClusterEvictionManager::ClusterEvictionManager(
    Event::Dispatcher& main_thread_dispatcher,
    ClusterEvictionManager::RemoveClusterCb remove_cluster)
    : remove_cluster_(std::move(remove_cluster)),
      sweep_timer_(main_thread_dispatcher.createTimer([this]() { sweep(); })) {}

void ClusterEvictionManager::setPolicy(absl::string_view cluster_name,
                                       std::chrono::milliseconds check_interval,
                                       ShouldEvictCb should_evict, OnEvictedCb on_evicted) {
  if (check_interval <= std::chrono::milliseconds::zero()) {
    // Eviction disabled for this cluster: drop any stale policy from an earlier registration.
    if (tracked_.erase(cluster_name) != 0) {
      invalidateCadence();
    }
    return;
  }

  Policy& policy = tracked_[cluster_name];
  if (policy.check_interval != check_interval) {
    policy.check_interval = check_interval;
    invalidateCadence();
  }
  policy.should_evict = std::move(should_evict);
  policy.on_evicted = std::move(on_evicted);
  armSweepTimer();
}

void ClusterEvictionManager::clearPolicy(absl::string_view cluster_name) {
  if (tracked_.erase(cluster_name) != 0) {
    invalidateCadence();
  }
}

void ClusterEvictionManager::armSweepTimer() {
  if (tracked_.empty() || sweep_timer_->enabled()) {
    return;
  }

  if (min_check_interval_ < std::chrono::milliseconds::zero()) {
    std::chrono::milliseconds cadence = std::chrono::milliseconds::max();
    for (const auto& [cluster_name, policy] : tracked_) {
      cadence = std::min(cadence, policy.check_interval);
    }
    min_check_interval_ = cadence;
  }
  sweep_timer_->enableTimer(min_check_interval_);
}

void ClusterEvictionManager::sweep() {
  // Collect the clusters to evict and drop them from tracking before invoking the removal callback.
  // remove_cluster_ removes the cluster from the cluster manager, which calls back into
  // clearPolicy(); dropping tracking up front keeps that re-entrant call a no-op and avoids
  // mutating tracked_ while iterating it.
  std::vector<std::pair<std::string, OnEvictedCb>> to_evict;
  for (auto it = tracked_.begin(); it != tracked_.end();) {
    auto& [cluster_name, policy] = *it;
    if (policy.should_evict(cluster_name)) {
      ENVOY_LOG(debug, "evicting cluster '{}'", cluster_name);
      to_evict.emplace_back(cluster_name, std::move(policy.on_evicted));
      tracked_.erase(it++);
      invalidateCadence();
    } else {
      ++it;
    }
  }

  for (auto& [cluster_name, on_evicted] : to_evict) {
    remove_cluster_(cluster_name);
    if (on_evicted != nullptr) {
      on_evicted(cluster_name);
    }
  }

  armSweepTimer();
}

} // namespace Upstream
} // namespace Envoy
