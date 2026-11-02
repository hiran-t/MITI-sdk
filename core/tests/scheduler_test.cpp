#include <gtest/gtest.h>

#include "scheduler.hpp"

using miti::Bytes;
using miti::detail::default_options;
using miti::detail::Delivery;
using miti::detail::Frame;
using miti::detail::Priority;
using miti::detail::Scheduler;
using miti::detail::SubscriptionOptions;
using namespace std::chrono_literals;

namespace
{

/// A frame whose head is one byte, to tell frames apart.
Frame frame(uint8_t tag)
{
  return Frame{true, Bytes{tag}, nullptr};
}

/// The tags of every frame the scheduler hands out, in order.
std::vector<int> drain(Scheduler & scheduler)
{
  std::vector<int> tags;
  while (auto next = scheduler.next()) tags.push_back(next->head.at(0));
  return tags;
}

const auto t0 = Scheduler::Clock::time_point{};

}  // namespace

TEST(Scheduler, SendsControlFramesBeforeMessages)
{
  Scheduler scheduler(10);
  scheduler.add(1, {});
  scheduler.push_message(1, frame(1), t0);
  scheduler.push_control(frame(9));
  EXPECT_EQ(drain(scheduler), (std::vector<int>{9, 1}));
}

TEST(Scheduler, SendsHigherPrioritiesFirst)
{
  Scheduler scheduler(10);
  scheduler.add(1, {0, Delivery::all, Priority::low});
  scheduler.add(2, {0, Delivery::all, Priority::normal});
  scheduler.add(3, {0, Delivery::all, Priority::high});
  scheduler.push_message(1, frame(1), t0);
  scheduler.push_message(2, frame(2), t0);
  scheduler.push_message(3, frame(3), t0);
  EXPECT_EQ(drain(scheduler), (std::vector<int>{3, 2, 1}));
}

TEST(Scheduler, TakesTurnsWithinAPriority)
{
  Scheduler scheduler(10);
  scheduler.add(1, {});
  scheduler.add(2, {});
  scheduler.push_message(1, frame(11), t0);
  scheduler.push_message(1, frame(12), t0);
  scheduler.push_message(1, frame(13), t0);
  scheduler.push_message(2, frame(21), t0);
  scheduler.push_message(2, frame(22), t0);
  EXPECT_EQ(drain(scheduler), (std::vector<int>{11, 21, 12, 22, 13}));
}

TEST(Scheduler, LatestDeliveryKeepsOnlyTheNewestWaitingMessage)
{
  Scheduler scheduler(10);
  scheduler.add(1, {0, Delivery::latest, Priority::normal});
  scheduler.push_message(1, frame(1), t0);
  scheduler.push_message(1, frame(2), t0);
  scheduler.push_message(1, frame(3), t0);
  EXPECT_EQ(drain(scheduler), (std::vector<int>{3}));
}

TEST(Scheduler, AllDeliveryDropsTheOldestBeyondTheLimit)
{
  Scheduler scheduler(2);
  scheduler.add(1, {});
  for (uint8_t tag = 1; tag <= 4; ++tag) scheduler.push_message(1, frame(tag), t0);
  EXPECT_EQ(drain(scheduler), (std::vector<int>{3, 4}));
}

TEST(Scheduler, MaxRateDropsMessagesThatComeTooSoon)
{
  Scheduler scheduler(10);
  scheduler.add(1, {10, Delivery::all, Priority::normal});  // one per 100 ms
  EXPECT_TRUE(scheduler.push_message(1, frame(1), t0));
  EXPECT_FALSE(scheduler.push_message(1, frame(2), t0 + 50ms));
  EXPECT_TRUE(scheduler.push_message(1, frame(3), t0 + 100ms));
  EXPECT_FALSE(scheduler.push_message(1, frame(4), t0 + 199ms));
  EXPECT_EQ(drain(scheduler), (std::vector<int>{1, 3}));
}

TEST(Scheduler, ChangedOptionsApplyToNewMessages)
{
  Scheduler scheduler(10);
  scheduler.add(1, {});
  scheduler.add(2, {});
  scheduler.options(2)->priority = Priority::high;
  scheduler.push_message(1, frame(1), t0);
  scheduler.push_message(2, frame(2), t0);
  EXPECT_EQ(drain(scheduler), (std::vector<int>{2, 1}));
  EXPECT_EQ(scheduler.options(3), nullptr);
}

TEST(Scheduler, ForgetsWhatARemovedSubscriptionHadWaiting)
{
  Scheduler scheduler(10);
  scheduler.add(1, {});
  scheduler.add(2, {});
  scheduler.push_message(1, frame(1), t0);
  scheduler.push_message(2, frame(2), t0);
  scheduler.remove(1);
  EXPECT_FALSE(scheduler.push_message(1, frame(3), t0));
  EXPECT_EQ(drain(scheduler), (std::vector<int>{2}));
}

TEST(Scheduler, DefaultOptionsFollowTheMessageType)
{
  const auto check = [](const std::string & type, Delivery delivery, Priority priority) {
      const SubscriptionOptions options = default_options(type);
      EXPECT_EQ(options.delivery, delivery) << type;
      EXPECT_EQ(options.priority, priority) << type;
      EXPECT_EQ(options.max_rate, 0) << type;
    };
  check("tf2_msgs/msg/TFMessage", Delivery::all, Priority::high);
  check("sensor_msgs/JointState", Delivery::all, Priority::high);
  check("sensor_msgs/msg/Image", Delivery::latest, Priority::low);
  check("sensor_msgs/msg/PointCloud2", Delivery::latest, Priority::low);
  check("foxglove_msgs/msg/CompressedVideo", Delivery::all, Priority::low);
  check("std_msgs/msg/String", Delivery::all, Priority::normal);
}
