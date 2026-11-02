#include "scheduler.hpp"

#include <set>
#include <string>

namespace miti::detail
{

SubscriptionOptions default_options(const std::string & schema_name)
{
  // Names with and without /msg/, as ROS 1 and ROS 2 tools write them.
  auto normalized = schema_name;
  if (const auto at = normalized.find("/msg/"); at != std::string::npos) normalized.erase(at, 4);
  static const std::set<std::string> realtime{
    "tf2_msgs/TFMessage", "sensor_msgs/JointState", "nav_msgs/Odometry"};
  static const std::set<std::string> heavy{
    "sensor_msgs/Image", "sensor_msgs/CompressedImage", "sensor_msgs/PointCloud2",
    "nav_msgs/OccupancyGrid", "nav2_msgs/Costmap", "map_msgs/OccupancyGridUpdate",
    "foxglove_msgs/RawImage", "foxglove_msgs/CompressedImage", "foxglove_msgs/PointCloud"};
  if (realtime.count(normalized)) return {0, Delivery::all, Priority::high};
  if (heavy.count(normalized)) return {0, Delivery::latest, Priority::low};
  // Dropping a video frame breaks the frames that depend on it.
  if (normalized == "foxglove_msgs/CompressedVideo") return {0, Delivery::all, Priority::low};
  return {};
}

void Scheduler::add(uint32_t subscription, SubscriptionOptions options)
{
  remove(subscription);
  queues_[subscription] = Queue{options, {}, false, std::nullopt};
}

void Scheduler::remove(uint32_t subscription)
{
  // A stale id left in ready_ is skipped by next().
  clear(subscription);
  queues_.erase(subscription);
}

void Scheduler::clear(uint32_t subscription)
{
  const auto found = queues_.find(subscription);
  if (found == queues_.end()) return;
  while (!found->second.pending.empty()) drop_front(found->second);
}

void Scheduler::drop_front(Queue & queue)
{
  queued_bytes_ -= queue.pending.front().size();
  queue.pending.pop_front();
  ++queue.dropped;
}

SubscriptionOptions * Scheduler::options(uint32_t subscription)
{
  const auto queue = queues_.find(subscription);
  return queue == queues_.end() ? nullptr : &queue->second.options;
}

void Scheduler::push_control(Frame frame)
{
  queued_bytes_ += frame.size();
  control_.push_back(std::move(frame));
}

bool Scheduler::admit(uint32_t subscription, Clock::time_point now)
{
  const auto found = queues_.find(subscription);
  if (found == queues_.end()) return false;
  auto & queue = found->second;
  // A message closer than 90 % of 1/maxRate to the last one sent is dropped. The tolerance keeps
  // a source at exactly maxRate from losing every message that arrives a little early.
  if (queue.options.max_rate > 0 && queue.last_accepted &&
    now - *queue.last_accepted < std::chrono::duration<double>(0.9 / queue.options.max_rate))
  {
    ++queue.dropped;
    return false;
  }
  queue.last_accepted = now;
  return true;
}

void Scheduler::enqueue(uint32_t subscription, Frame frame, Clock::time_point now)
{
  const auto found = queues_.find(subscription);
  if (found == queues_.end()) return;
  auto & queue = found->second;
  if (queue.options.delivery == Delivery::latest) {
    while (!queue.pending.empty()) drop_front(queue);
  } else if (queue.pending.size() >= queue_limit_) {
    drop_front(queue);
  }
  frame.queued = now;
  queued_bytes_ += frame.size();
  queue.pending.push_back(std::move(frame));
  if (!queue.ready) {
    ready_[static_cast<size_t>(queue.options.priority)].push_back(subscription);
    queue.ready = true;
  }
}

void Scheduler::count_dropped(uint32_t subscription)
{
  const auto found = queues_.find(subscription);
  if (found != queues_.end()) ++found->second.dropped;
}

bool Scheduler::push_message(uint32_t subscription, Frame frame, Clock::time_point now)
{
  if (!admit(subscription, now)) return false;
  enqueue(subscription, std::move(frame), now);
  return true;
}

std::vector<Scheduler::Report> Scheduler::report(Clock::time_point now)
{
  std::vector<Report> reports;
  for (auto & [id, queue] : queues_) {
    const double oldest = queue.pending.empty() ? 0 :
      std::chrono::duration<double, std::milli>(now - queue.pending.front().queued).count();
    reports.push_back({id, queue.sent, queue.sent_bytes, queue.dropped, oldest});
    queue.sent = queue.sent_bytes = queue.dropped = 0;
  }
  return reports;
}

std::optional<Frame> Scheduler::next()
{
  if (!control_.empty()) {
    auto frame = std::move(control_.front());
    control_.pop_front();
    queued_bytes_ -= frame.size();
    return frame;
  }
  for (auto & ready : ready_) {
    while (!ready.empty()) {
      const uint32_t subscription = ready.front();
      ready.pop_front();
      const auto found = queues_.find(subscription);
      if (found == queues_.end()) continue;
      auto & queue = found->second;
      queue.ready = false;
      if (queue.pending.empty()) continue;
      auto frame = std::move(queue.pending.front());
      queue.pending.pop_front();
      queued_bytes_ -= frame.size();
      ++queue.sent;
      queue.sent_bytes += frame.size();
      // Its turn is over: back of the line, at its current priority.
      if (!queue.pending.empty()) {
        ready_[static_cast<size_t>(queue.options.priority)].push_back(subscription);
        queue.ready = true;
      }
      return frame;
    }
  }
  return std::nullopt;
}

}  // namespace miti::detail
