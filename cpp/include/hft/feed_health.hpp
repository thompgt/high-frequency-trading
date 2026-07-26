// Market data session health: sequence validation and a staleness watchdog.
//
// Why a missed message is not a small problem
// -------------------------------------------
// The order book is built by applying every message in order. Miss one and the
// book is not slightly stale, it is *wrong* -- and it stays wrong, silently,
// forever, because nothing later in the stream corrects it. An order that was
// added and never cancelled sits in your book as phantom liquidity; a cancel
// you missed leaves depth that is not there. The strategy then quotes against
// a book that does not exist, and the first sign of trouble is the fills.
//
// This is why every real feed carries a sequence number and why every real feed
// handler checks it. The correct response to a gap is not to carry on: it is to
// stop trading and re-synchronise from a snapshot. This engine has no snapshot
// channel, so it does the honest half of that -- it stops.
//
// The other failure the watchdog covers is a feed that goes quiet. A silent
// feed and a calm market look identical from inside the process, and the
// difference matters enormously: one means there is nothing to do, the other
// means your book is frozen at whatever it last saw while the market moves
// away from it. Anything past a threshold is treated as a fault.
//
// Both checks are on the tick path, so both are branch-and-compare cheap.
#pragma once

#include <cstdint>
#include <string>

#include "hft/types.hpp"

namespace hft {

// What the monitor made of one message.
enum class FeedStatus : std::uint8_t {
  Ok = 0,
  Gap,         // sequence jumped forward: messages were lost
  Duplicate,   // sequence repeated: benign on an arbitrated A/B feed
  Reordered,   // sequence went backwards: arrived late, after a later message
  Unsequenced, // the feed does not sequence, and we required that it does
  kCount
};

const char* to_string(FeedStatus s);

struct FeedHealthConfig {
  // Validate sequence numbers. Off only for feeds that genuinely have none,
  // in which case you are trusting the transport completely.
  bool require_sequence = true;

  // Treat a gap as fatal and halt. On by default, because continuing to trade
  // on a book you know is missing updates is the more dangerous choice -- and
  // it is the choice that stays wrong silently.
  bool halt_on_gap = true;

  // Gaps of up to this many messages are counted but tolerated. Zero means no
  // gap is tolerable, which is the right setting for a book-building feed.
  // A trade-print-only feed can afford a small allowance.
  std::uint64_t tolerated_gap = 0;

  // Halt if no message arrives for this long. Zero disables the watchdog.
  // Deliberately not defaulted to a number: the right value depends entirely on
  // how busy the instrument is, and a wrong one is worse than none.
  Nanos stale_after_ns = 0;
};

struct FeedHealthStats {
  std::uint64_t messages = 0;
  std::uint64_t gaps = 0;             // number of gap events
  std::uint64_t messages_missing = 0; // total messages lost across all gaps
  std::uint64_t duplicates = 0;
  std::uint64_t reordered = 0;
  std::uint64_t unsequenced = 0;
  std::uint64_t largest_gap = 0;
  std::uint64_t stale_events = 0;
};

class FeedMonitor {
 public:
  FeedMonitor();
  explicit FeedMonitor(FeedHealthConfig config);

  // Validates one message. `now_ns` is injected rather than read from a clock
  // so the watchdog can be driven deterministically in tests.
  FeedStatus on_tick(const Tick& tick, Nanos now_ns);

  // True when nothing has arrived for longer than stale_after_ns. Call this
  // from the idle path -- a feed that has gone silent will not call on_tick()
  // to tell you so. Counts a stale event on the transition into staleness, not
  // once per poll.
  bool check_stale(Nanos now_ns);

  // True when the session is in a state where trading should not continue:
  // an intolerable gap, or a stale feed. Sticky -- resynchronise() clears it.
  bool faulted() const { return faulted_; }
  FeedStatus fault_reason() const { return fault_reason_; }

  // Declares the session re-synchronised (in a real deployment, after
  // rebuilding the book from a snapshot). Deliberately explicit: a gap must
  // never clear itself just because the next message happened to arrive in
  // order.
  void resynchronise(std::uint64_t from_sequence);

  std::uint64_t last_sequence() const { return last_sequence_; }
  Nanos last_message_ns() const { return last_message_ns_; }
  bool healthy() const { return !faulted_ && stats_.gaps == 0; }

  const FeedHealthStats& stats() const { return stats_; }
  const FeedHealthConfig& config() const { return cfg_; }
  std::string summary() const;
  void reset();

 private:
  FeedHealthConfig cfg_;
  FeedHealthStats stats_;
  std::uint64_t last_sequence_ = 0;
  Nanos last_message_ns_ = 0;
  bool seen_any_ = false;
  bool stale_ = false;
  bool faulted_ = false;
  FeedStatus fault_reason_ = FeedStatus::Ok;
};

}  // namespace hft
