#include <chrono>
#include <string>

#include "envoy/event/dispatcher.h"

#include "source/common/upstream/cluster_eviction.h"

#include "test/mocks/event/mocks.h"

#include "absl/container/flat_hash_set.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::NiceMock;

namespace Envoy {
namespace Upstream {
namespace {

constexpr std::chrono::milliseconds kInterval{100};

// Drives ClusterEvictionManager with hand-controlled predicates so tests can decide which clusters
// report themselves evictable and observe which get removed, without a real cluster manager.
class ClusterEvictionManagerTest : public testing::Test {
public:
  // Installs the mock timer before the manager is constructed so the manager's createTimer() call
  // in its constructor returns it.
  ClusterEvictionManagerTest() : timer_(new NiceMock<Event::MockTimer>(&dispatcher_)) {
    manager_ = std::make_unique<ClusterEvictionManager>(
        dispatcher_, [this](absl::string_view name) {
      removed_.insert(std::string(name));
      // Model the real cluster manager: removing a cluster re-enters through clearPolicy().
      manager_->clearPolicy(name);
    });
  }

  NiceMock<Event::MockDispatcher> dispatcher_;
  Event::MockTimer* timer_;
  absl::flat_hash_set<std::string> removed_;
  ClusterEvictionManagerPtr manager_;
};

// A cluster whose predicate returns true is removed on the next sweep, and the sweep arms at the
// configured interval.
TEST_F(ClusterEvictionManagerTest, EvictsWhenPredicateTrue) {
  bool evictable = false;
  EXPECT_CALL(*timer_, enableTimer(kInterval, _)).Times(2);
  manager_->setPolicy("foo", kInterval, [&evictable](absl::string_view cluster_name) {
    EXPECT_EQ(cluster_name, "foo");
    return evictable;
  }, nullptr);

  timer_->invokeCallback();
  EXPECT_TRUE(removed_.empty()); // predicate false -> kept

  evictable = true;
  timer_->invokeCallback();
  EXPECT_THAT(removed_, testing::Contains("foo"));
}

// A non-positive interval clears/never-arms.
TEST_F(ClusterEvictionManagerTest, DisabledIntervalTracksNothing) {
  manager_->setPolicy("foo", std::chrono::milliseconds::zero(), [](absl::string_view) { return true; }, nullptr);
  EXPECT_FALSE(timer_->enabled());
}

// on_evicted fires when the cluster is evicted, and reclamation is re-entrancy safe (the remove
// callback calls clearPolicy mid-sweep).
TEST_F(ClusterEvictionManagerTest, InvokesOnEvictedAndIsReentrancySafe) {
  bool evicted = false;
  manager_->setPolicy("foo", kInterval, [](absl::string_view cluster_name) {
    EXPECT_EQ(cluster_name, "foo");
    return true;
  }, [&evicted](absl::string_view cluster_name) {
    EXPECT_EQ(cluster_name, "foo");
    evicted = true;
  });

  timer_->invokeCallback();
  EXPECT_THAT(removed_, testing::Contains("foo"));
  EXPECT_TRUE(evicted);
}

// The sweep evaluates every tracked cluster; only those whose predicate is true are removed.
TEST_F(ClusterEvictionManagerTest, EvictsOnlyMatchingClusters) {
  manager_->setPolicy("stale", kInterval, [](absl::string_view) { return true; }, nullptr);
  manager_->setPolicy("live", kInterval, [](absl::string_view) { return false; }, nullptr);

  timer_->invokeCallback();
  EXPECT_THAT(removed_, testing::Contains("stale"));
  EXPECT_THAT(removed_, testing::Not(testing::Contains("live")));
}

// clearPolicy stops a cluster from being evicted.
TEST_F(ClusterEvictionManagerTest, ClearedPolicyIsNotEvaluated) {
  manager_->setPolicy("foo", kInterval, [](absl::string_view) { return true; }, nullptr);
  manager_->clearPolicy("foo");

  timer_->invokeCallback();
  EXPECT_TRUE(removed_.empty());
}

} // namespace
} // namespace Upstream
} // namespace Envoy
