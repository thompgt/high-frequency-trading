// Feed session health tests.
//
// The property being defended here is that a book built from an incomplete
// message stream is wrong and stays wrong. So most of these are about what
// happens when the stream is not clean: gaps, duplicates, late arrivals, and a
// feed that simply stops.

#include <string>

#include "hft/engine.hpp"
#include "hft/feed.hpp"
#include "hft/feed_health.hpp"
#include "test_harness.hpp"

using namespace hft;

namespace {

Tick seq_tick(std::uint64_t sequence, Price px = 10000) {
  Tick t{};
  t.symbol = 0;
  t.type = TickType::AddOrder;
  t.side = Side::Buy;
  t.price = px;
  t.quantity = 10;
  t.order_id = sequence;
  t.sequence = sequence;
  return t;
}

}  // namespace

// ============================================================== sequencing

TEST(feed_monitor_accepts_a_contiguous_stream) {
  FeedMonitor monitor;
  for (std::uint64_t i = 1; i <= 1000; ++i) {
    CHECK_EQ(int(monitor.on_tick(seq_tick(i), static_cast<Nanos>(i))), int(FeedStatus::Ok));
  }
  CHECK(monitor.healthy());
  CHECK_FALSE(monitor.faulted());
  CHECK_EQ(monitor.stats().messages, std::uint64_t(1000));
  CHECK_EQ(monitor.stats().gaps, std::uint64_t(0));
  CHECK_EQ(monitor.last_sequence(), std::uint64_t(1000));
}

TEST(feed_monitor_starts_from_whatever_sequence_it_first_sees) {
  // Joining a feed mid-session is normal; it is not a gap.
  FeedMonitor monitor;
  CHECK_EQ(int(monitor.on_tick(seq_tick(50'000), 1)), int(FeedStatus::Ok));
  CHECK_EQ(int(monitor.on_tick(seq_tick(50'001), 2)), int(FeedStatus::Ok));
  CHECK_EQ(monitor.stats().gaps, std::uint64_t(0));
}

TEST(feed_monitor_detects_a_gap_and_counts_what_was_lost) {
  FeedMonitor monitor;
  monitor.on_tick(seq_tick(1), 1);
  monitor.on_tick(seq_tick(2), 2);
  CHECK_EQ(int(monitor.on_tick(seq_tick(9), 3)), int(FeedStatus::Gap));

  CHECK_EQ(monitor.stats().gaps, std::uint64_t(1));
  CHECK_EQ(monitor.stats().messages_missing, std::uint64_t(6));  // 3..8
  CHECK_EQ(monitor.stats().largest_gap, std::uint64_t(6));
  CHECK_FALSE(monitor.healthy());
  // The book is now missing six messages, so the session is faulted.
  CHECK(monitor.faulted());
  CHECK_EQ(int(monitor.fault_reason()), int(FeedStatus::Gap));

  // The stream continues from the new position rather than reporting a second
  // gap for every subsequent message.
  CHECK_EQ(int(monitor.on_tick(seq_tick(10), 4)), int(FeedStatus::Ok));
  CHECK_EQ(monitor.stats().gaps, std::uint64_t(1));
}

TEST(feed_monitor_treats_a_repeat_as_a_duplicate_not_a_gap) {
  // Arbitrated A/B feeds deliver every message twice by design.
  FeedMonitor monitor;
  monitor.on_tick(seq_tick(1), 1);
  monitor.on_tick(seq_tick(2), 2);
  CHECK_EQ(int(monitor.on_tick(seq_tick(2), 3)), int(FeedStatus::Duplicate));

  CHECK_EQ(monitor.stats().duplicates, std::uint64_t(1));
  CHECK_EQ(monitor.stats().gaps, std::uint64_t(0));
  CHECK_FALSE(monitor.faulted());  // benign; must not stop trading
  CHECK_EQ(int(monitor.on_tick(seq_tick(3), 4)), int(FeedStatus::Ok));
}

TEST(feed_monitor_detects_a_message_that_arrives_late) {
  FeedMonitor monitor;
  monitor.on_tick(seq_tick(1), 1);
  monitor.on_tick(seq_tick(5), 2);  // gap
  CHECK_EQ(int(monitor.on_tick(seq_tick(3), 3)), int(FeedStatus::Reordered));
  CHECK_EQ(monitor.stats().reordered, std::uint64_t(1));
  // A late message must not roll the position backwards.
  CHECK_EQ(monitor.last_sequence(), std::uint64_t(5));
}

TEST(feed_monitor_reports_an_unsequenced_feed_without_halting) {
  // Requiring sequencing from a feed that has none is a configuration
  // mismatch. Counting it is right; turning it into an outage is not.
  FeedMonitor monitor;
  Tick t = seq_tick(0);
  CHECK_EQ(int(monitor.on_tick(t, 1)), int(FeedStatus::Unsequenced));
  CHECK_EQ(int(monitor.on_tick(t, 2)), int(FeedStatus::Unsequenced));
  CHECK_EQ(monitor.stats().unsequenced, std::uint64_t(2));
  CHECK_FALSE(monitor.faulted());
}

TEST(feed_monitor_can_be_told_not_to_check_sequencing) {
  FeedHealthConfig cfg;
  cfg.require_sequence = false;
  FeedMonitor monitor(cfg);
  monitor.on_tick(seq_tick(1), 1);
  CHECK_EQ(int(monitor.on_tick(seq_tick(9999), 2)), int(FeedStatus::Ok));
  CHECK_EQ(monitor.stats().gaps, std::uint64_t(0));
  CHECK_FALSE(monitor.faulted());
}

TEST(feed_monitor_tolerates_gaps_up_to_the_configured_allowance) {
  FeedHealthConfig cfg;
  cfg.tolerated_gap = 3;
  FeedMonitor monitor(cfg);

  monitor.on_tick(seq_tick(1), 1);
  // Three lost messages: counted, but within the allowance.
  CHECK_EQ(int(monitor.on_tick(seq_tick(5), 2)), int(FeedStatus::Gap));
  CHECK_EQ(monitor.stats().messages_missing, std::uint64_t(3));
  CHECK_FALSE(monitor.faulted());

  // Four is past it.
  CHECK_EQ(int(monitor.on_tick(seq_tick(10), 3)), int(FeedStatus::Gap));
  CHECK(monitor.faulted());
}

TEST(feed_monitor_can_be_configured_not_to_halt_on_a_gap) {
  FeedHealthConfig cfg;
  cfg.halt_on_gap = false;
  FeedMonitor monitor(cfg);
  monitor.on_tick(seq_tick(1), 1);
  CHECK_EQ(int(monitor.on_tick(seq_tick(500), 2)), int(FeedStatus::Gap));
  CHECK_EQ(monitor.stats().gaps, std::uint64_t(1));
  CHECK_FALSE(monitor.faulted());
  // Still not "healthy": the gap happened and is still reported.
  CHECK_FALSE(monitor.healthy());
}

// ============================================================== resync

TEST(feed_monitor_fault_is_sticky_until_explicitly_resynchronised) {
  FeedMonitor monitor;
  monitor.on_tick(seq_tick(1), 1);
  monitor.on_tick(seq_tick(100), 2);
  CHECK(monitor.faulted());

  // Messages continuing to arrive in order does NOT mean the book was repaired.
  for (std::uint64_t i = 101; i < 200; ++i) monitor.on_tick(seq_tick(i), 3);
  CHECK(monitor.faulted());

  monitor.resynchronise(199);
  CHECK_FALSE(monitor.faulted());
  CHECK_EQ(int(monitor.fault_reason()), int(FeedStatus::Ok));
  CHECK_EQ(int(monitor.on_tick(seq_tick(200), 4)), int(FeedStatus::Ok));
}

// ============================================================== staleness

TEST(feed_monitor_watchdog_fires_when_the_feed_goes_quiet) {
  FeedHealthConfig cfg;
  cfg.stale_after_ns = 1000;
  FeedMonitor monitor(cfg);

  monitor.on_tick(seq_tick(1), 10'000);
  CHECK_FALSE(monitor.check_stale(10'500));  // still within the window
  CHECK(monitor.check_stale(11'500));
  CHECK(monitor.faulted());
  CHECK_EQ(monitor.stats().stale_events, std::uint64_t(1));
}

TEST(feed_monitor_counts_one_stale_event_per_outage_not_per_poll) {
  // A watchdog checked in a tight loop must not report millions of events for
  // one silent feed.
  FeedHealthConfig cfg;
  cfg.stale_after_ns = 1000;
  FeedMonitor monitor(cfg);
  monitor.on_tick(seq_tick(1), 10'000);

  for (Nanos t = 11'500; t < 20'000; t += 10) monitor.check_stale(t);
  CHECK_EQ(monitor.stats().stale_events, std::uint64_t(1));

  // A message arriving clears staleness; a second outage is a second event.
  monitor.on_tick(seq_tick(2), 20'000);
  CHECK_FALSE(monitor.check_stale(20'100));
  CHECK(monitor.check_stale(21'500));
  CHECK_EQ(monitor.stats().stale_events, std::uint64_t(2));
}

TEST(feed_monitor_watchdog_is_off_by_default) {
  // There is no universally right staleness threshold, so there is no default
  // one -- a wrong value is worse than none.
  FeedMonitor monitor;
  monitor.on_tick(seq_tick(1), 0);
  CHECK_FALSE(monitor.check_stale(1'000'000'000'000LL));
  CHECK_FALSE(monitor.faulted());
}

TEST(feed_monitor_watchdog_does_not_fire_before_the_first_message) {
  FeedHealthConfig cfg;
  cfg.stale_after_ns = 1000;
  FeedMonitor monitor(cfg);
  // Nothing has started yet; that is not an outage.
  CHECK_FALSE(monitor.check_stale(1'000'000));
  CHECK_FALSE(monitor.faulted());
}

// ============================================================ bookkeeping

TEST(feed_monitor_reset_clears_everything) {
  FeedMonitor monitor;
  monitor.on_tick(seq_tick(1), 1);
  monitor.on_tick(seq_tick(50), 2);
  CHECK(monitor.faulted());

  monitor.reset();
  CHECK_FALSE(monitor.faulted());
  CHECK(monitor.healthy());
  CHECK_EQ(monitor.stats().messages, std::uint64_t(0));
  CHECK_EQ(monitor.last_sequence(), std::uint64_t(0));
}

TEST(feed_monitor_summary_names_the_fault) {
  FeedMonitor monitor;
  monitor.on_tick(seq_tick(1), 1);
  monitor.on_tick(seq_tick(20), 2);
  const std::string text = monitor.summary();
  CHECK(text.find("FAULTED") != std::string::npos);
  CHECK(text.find("cannot be trusted") != std::string::npos);
  CHECK(text.find("sequence gaps") != std::string::npos);
}

TEST(feed_status_strings_are_distinct) {
  const FeedStatus all[] = {FeedStatus::Ok, FeedStatus::Gap, FeedStatus::Duplicate,
                            FeedStatus::Reordered, FeedStatus::Unsequenced};
  for (const FeedStatus a : all) {
    for (const FeedStatus b : all) {
      if (a == b) continue;
      CHECK_NE(std::string(to_string(a)), std::string(to_string(b)));
    }
  }
}

// ============================================================ feed plumbing

TEST(synthetic_feed_sequences_its_messages_from_one) {
  SyntheticFeed::Params p;
  p.total_events = 100;
  SyntheticFeed feed(p);

  Tick t;
  std::uint64_t expected = 1;
  while (feed.next(t)) {
    CHECK_EQ(t.sequence, expected);
    ++expected;
  }
  CHECK_EQ(expected, std::uint64_t(101));
}

TEST(replay_capture_round_trips_sequence_numbers) {
  SyntheticFeed::Params p;
  p.total_events = 500;
  p.seed = 0xFEEDFACEULL;
  const std::string path = "build/replay_sequence.csv";
  CHECK(SyntheticFeed::write_replay_csv(path, p, 500));

  CsvReplayFeed replay(path);
  CHECK(replay.ok());
  CHECK_EQ(replay.size(), std::size_t(500));

  FeedMonitor monitor;
  Tick t;
  while (replay.next(t)) monitor.on_tick(t, t.ingest_ts_ns);
  // A capture written by this build must replay as a perfectly clean session.
  CHECK(monitor.healthy());
  CHECK_EQ(monitor.stats().messages, std::uint64_t(500));
  CHECK_EQ(monitor.stats().gaps, std::uint64_t(0));
}

TEST(replay_tolerates_a_capture_written_before_sequence_numbers_existed) {
  // Old captures have seven columns. They must still replay -- and must read
  // as unsequenced rather than as one enormous gap.
  const std::string path = "build/replay_legacy.csv";
  {
    std::FILE* f = std::fopen(path.c_str(), "w");
    CHECK(f != nullptr);
    std::fprintf(f, "symbol,type,side,price,quantity,order_id,source_ts_ns\n");
    for (int i = 1; i <= 20; ++i) {
      std::fprintf(f, "0,2,0,10000,%d,%d,%d\n", i, i, i * 100);
    }
    std::fclose(f);
  }

  CsvReplayFeed replay(path);
  CHECK(replay.ok());
  CHECK_EQ(replay.size(), std::size_t(20));

  FeedMonitor monitor;
  Tick t;
  while (replay.next(t)) {
    CHECK_EQ(t.sequence, std::uint64_t(0));
    monitor.on_tick(t, t.ingest_ts_ns);
  }
  CHECK_EQ(monitor.stats().unsequenced, std::uint64_t(20));
  CHECK_EQ(monitor.stats().gaps, std::uint64_t(0));
  CHECK_FALSE(monitor.faulted());
}

// ====================================================== engine integration

namespace {

EngineConfig health_engine_config() {
  EngineConfig cfg;
  cfg.threaded = false;
  cfg.fast_window = 3;
  cfg.slow_window = 8;
  cfg.order_quantity = 5;
  cfg.record_curve = false;
  cfg.risk.max_orders_per_second = 1'000'000'000u;
  return cfg;
}

}  // namespace

TEST(engine_runs_a_clean_feed_without_faulting) {
  Engine engine(health_engine_config());
  SyntheticFeed::Params p;
  p.total_events = 80'000;
  SyntheticFeed feed(p);

  const EngineStats stats = engine.run(feed);
  CHECK(engine.feed_monitor().healthy());
  CHECK_EQ(stats.stale_messages, std::uint64_t(0));
  CHECK_EQ(stats.ticks_while_faulted, std::uint64_t(0));
  CHECK_FALSE(engine.risk().halted());
  CHECK(stats.orders_sent > 0);
}

TEST(engine_stops_trading_when_the_feed_gaps) {
  // The point of the whole subsystem: once messages are known to be missing,
  // the book cannot be trusted and nothing may trade off it.
  Engine engine(health_engine_config());
  EngineStats stats;

  // Warm the strategy up on a clean stream so it is actually producing signals.
  for (std::uint64_t i = 1; i <= 2000; ++i) {
    Tick t = seq_tick(i, 10000 + static_cast<Price>(i % 20));
    t.ingest_ts_ns = static_cast<Nanos>(i);
    engine.process(t, stats);
  }
  const std::uint64_t orders_before = stats.orders_sent;
  CHECK_FALSE(engine.risk().halted());

  // Now lose a block of messages.
  Tick gapped = seq_tick(5000, 10010);
  gapped.ingest_ts_ns = 3000;
  engine.process(gapped, stats);

  CHECK(engine.feed_monitor().faulted());
  CHECK(engine.risk().halted());
  CHECK(stats.halted);

  // Keep feeding it: no further orders may go out.
  for (std::uint64_t i = 5001; i <= 7000; ++i) {
    Tick t = seq_tick(i, 10000 + static_cast<Price>(i % 20));
    t.ingest_ts_ns = static_cast<Nanos>(i);
    engine.process(t, stats);
  }
  CHECK_EQ(stats.orders_sent, orders_before);
  CHECK(stats.ticks_while_faulted > 0);
}

TEST(engine_drops_duplicate_and_reordered_messages_before_the_book) {
  // Applying a duplicate add would create liquidity that does not exist.
  Engine engine(health_engine_config());
  EngineStats stats;

  Tick t1 = seq_tick(1, 10000);
  t1.ingest_ts_ns = 1;
  engine.process(t1, stats);
  const std::size_t after_first = engine.book().live_order_count();

  // Same message again: must not reach the book.
  engine.process(t1, stats);
  CHECK_EQ(engine.book().live_order_count(), after_first);
  CHECK_EQ(stats.stale_messages, std::uint64_t(1));

  Tick t3 = seq_tick(3, 10001);
  t3.ingest_ts_ns = 3;
  engine.process(t3, stats);
  // A message from before the one we just applied is stale by definition.
  Tick late = seq_tick(2, 10002);
  late.order_id = 999;
  late.ingest_ts_ns = 4;
  const std::size_t before_late = engine.book().live_order_count();
  engine.process(late, stats);
  CHECK_EQ(engine.book().live_order_count(), before_late);
  CHECK_EQ(stats.stale_messages, std::uint64_t(2));
}

TEST(engine_with_sequence_checking_disabled_ignores_gaps) {
  // Escape hatch for feeds that genuinely have no sequencing.
  EngineConfig cfg = health_engine_config();
  cfg.feed_health.require_sequence = false;
  Engine engine(cfg);
  EngineStats stats;

  for (std::uint64_t i = 1; i <= 500; ++i) {
    Tick t = seq_tick(i * 17, 10000 + static_cast<Price>(i % 20));  // wildly non-contiguous
    t.ingest_ts_ns = static_cast<Nanos>(i);
    engine.process(t, stats);
  }
  CHECK_FALSE(engine.feed_monitor().faulted());
  CHECK_FALSE(engine.risk().halted());
  CHECK_EQ(stats.stale_messages, std::uint64_t(0));
}
