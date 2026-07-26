#include "hft/feed_health.hpp"

#include <cstdio>

namespace hft {

const char* to_string(FeedStatus s) {
  switch (s) {
    case FeedStatus::Ok: return "ok";
    case FeedStatus::Gap: return "gap";
    case FeedStatus::Duplicate: return "duplicate";
    case FeedStatus::Reordered: return "reordered";
    case FeedStatus::Unsequenced: return "unsequenced";
    case FeedStatus::kCount: break;
  }
  return "unknown";
}

FeedMonitor::FeedMonitor() : FeedMonitor(FeedHealthConfig()) {}

FeedMonitor::FeedMonitor(FeedHealthConfig config) : cfg_(config) {}

FeedStatus FeedMonitor::on_tick(const Tick& tick, Nanos now_ns) {
  ++stats_.messages;
  last_message_ns_ = now_ns;
  stale_ = false;

  if (!cfg_.require_sequence) return FeedStatus::Ok;

  if (tick.sequence == 0) {
    // We were told to check sequencing and the feed did not supply it. Counted
    // rather than fatal: it is a configuration mismatch, not a lost message,
    // and halting on it would turn a wrong setting into an outage.
    ++stats_.unsequenced;
    return FeedStatus::Unsequenced;
  }

  if (!seen_any_) {
    seen_any_ = true;
    last_sequence_ = tick.sequence;
    return FeedStatus::Ok;
  }

  const std::uint64_t expected = last_sequence_ + 1;
  if (tick.sequence == expected) {
    last_sequence_ = tick.sequence;
    return FeedStatus::Ok;
  }

  if (tick.sequence == last_sequence_) {
    // Arbitrated A/B feeds deliver the same message twice by design. Counting
    // it is useful; reacting to it is not.
    ++stats_.duplicates;
    return FeedStatus::Duplicate;
  }

  if (tick.sequence < last_sequence_) {
    // Arrived after a message that supersedes it. Applying it would undo newer
    // state, so the caller must drop it.
    ++stats_.reordered;
    return FeedStatus::Reordered;
  }

  // Forward jump: messages between `expected` and this one are gone.
  const std::uint64_t missing = tick.sequence - expected;
  ++stats_.gaps;
  stats_.messages_missing += missing;
  if (missing > stats_.largest_gap) stats_.largest_gap = missing;
  last_sequence_ = tick.sequence;

  if (cfg_.halt_on_gap && missing > cfg_.tolerated_gap) {
    faulted_ = true;
    fault_reason_ = FeedStatus::Gap;
  }
  return FeedStatus::Gap;
}

bool FeedMonitor::check_stale(Nanos now_ns) {
  if (cfg_.stale_after_ns <= 0) return false;
  if (!seen_any_ && last_message_ns_ == 0) return false;  // nothing has started yet
  if (now_ns - last_message_ns_ < cfg_.stale_after_ns) return false;

  if (!stale_) {
    // Count the transition, not every poll -- otherwise a watchdog checked in a
    // tight loop reports millions of "events" for one silent feed.
    stale_ = true;
    ++stats_.stale_events;
    faulted_ = true;
    fault_reason_ = FeedStatus::Gap;
  }
  return true;
}

void FeedMonitor::resynchronise(std::uint64_t from_sequence) {
  last_sequence_ = from_sequence;
  seen_any_ = from_sequence != 0;
  faulted_ = false;
  fault_reason_ = FeedStatus::Ok;
  stale_ = false;
}

void FeedMonitor::reset() {
  stats_ = FeedHealthStats{};
  last_sequence_ = 0;
  last_message_ns_ = 0;
  seen_any_ = false;
  stale_ = false;
  faulted_ = false;
  fault_reason_ = FeedStatus::Ok;
}

std::string FeedMonitor::summary() const {
  char buf[640];
  std::snprintf(buf, sizeof(buf),
                "messages received   : %llu\n"
                "sequence gaps       : %llu  (%llu message(s) lost, largest gap %llu)\n"
                "duplicates          : %llu\n"
                "reordered           : %llu\n"
                "unsequenced         : %llu\n"
                "stale events        : %llu\n"
                "session state       : %s\n",
                (unsigned long long)stats_.messages, (unsigned long long)stats_.gaps,
                (unsigned long long)stats_.messages_missing,
                (unsigned long long)stats_.largest_gap, (unsigned long long)stats_.duplicates,
                (unsigned long long)stats_.reordered, (unsigned long long)stats_.unsequenced,
                (unsigned long long)stats_.stale_events,
                faulted_ ? "FAULTED -- the book is missing updates and cannot be trusted"
                         : "ok");
  return std::string(buf);
}

}  // namespace hft
