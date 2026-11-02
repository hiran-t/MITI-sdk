// What one connection sends next (protocol.md, miti.subscriptionOptions and Backpressure).

#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "miti/server.hpp"

namespace miti::detail
{

enum class Priority { high = 0, normal = 1, low = 2 };
enum class Delivery { all, latest };

struct SubscriptionOptions
{
  double max_rate = 0;  // messages per second; 0 for no limit
  Delivery delivery = Delivery::all;
  Priority priority = Priority::normal;
};

/// The options protocol.md gives a subscription until the client sends its own.
SubscriptionOptions default_options(const std::string & schema_name);

/// One WebSocket message: a head the connection owns, and a body that may be shared with every
/// other connection (a message's bytes are never copied per subscriber).
struct Frame
{
  bool binary = true;
  Bytes head;
  std::shared_ptr<const Bytes> body;
  /// When it was queued, for miti.stats.
  std::chrono::steady_clock::time_point queued{};

  size_t size() const {return head.size() + (body ? body->size() : 0);}
};

/// Holds what a connection has to send and picks the next frame: control frames first, then
/// subscriptions by priority, round-robin within a priority. The connection writes one frame at a
/// time, so messages wait here, where a newer one can still replace them, rather than in the
/// socket.
class Scheduler
{
public:
  using Clock = std::chrono::steady_clock;

  explicit Scheduler(size_t queue_limit) : queue_limit_(queue_limit) {}

  void add(uint32_t subscription, SubscriptionOptions options);
  void remove(uint32_t subscription);
  bool has(uint32_t subscription) const {return queues_.count(subscription) > 0;}
  /// Null for an unknown subscription. Changes apply to messages pushed from then on.
  SubscriptionOptions * options(uint32_t subscription);

  /// JSON operations, service and parameter replies, time: sent before any message.
  void push_control(Frame frame);
  /// Whether a message of a subscription may go now under its maxRate; one that may not is
  /// counted as dropped. A transformed message is admitted before it is transformed.
  bool admit(uint32_t subscription, Clock::time_point now);
  /// Queues an admitted message: a newer one replaces it under `latest` delivery, and the oldest
  /// goes when an `all` queue is full.
  void enqueue(uint32_t subscription, Frame frame, Clock::time_point now);
  /// Counts an admitted message that never reached the queue, as one its transform could not make.
  void count_dropped(uint32_t subscription);
  /// admit, then enqueue. Returns whether the message was queued.
  bool push_message(uint32_t subscription, Frame frame, Clock::time_point now);
  /// Drops what a subscription has waiting, as when the format of its messages changes.
  void clear(uint32_t subscription);

  std::optional<Frame> next();

  /// What a subscription sent and dropped since the last report.
  struct Report
  {
    uint32_t subscription;
    uint64_t sent, sent_bytes, dropped;
    double oldest_queued_ms;
  };
  std::vector<Report> report(Clock::time_point now);
  /// The bytes of every frame waiting.
  size_t queued_bytes() const {return queued_bytes_;}

private:
  struct Queue
  {
    SubscriptionOptions options;
    std::deque<Frame> pending;
    bool ready = false;  // listed in ready_
    std::optional<Clock::time_point> last_accepted;
    uint64_t sent = 0, sent_bytes = 0, dropped = 0;
  };

  void drop_front(Queue & queue);

  size_t queue_limit_;
  size_t queued_bytes_ = 0;
  std::deque<Frame> control_;
  std::map<uint32_t, Queue> queues_;
  /// Subscriptions with pending messages, by priority, in turn order.
  std::array<std::deque<uint32_t>, 3> ready_;
};

}  // namespace miti::detail
