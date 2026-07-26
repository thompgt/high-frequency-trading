// The ack-timeout sweep.
//
// An order with no response is the worst state to be in: it may be resting at
// the venue or it may have died on the wire, and those demand opposite
// actions. The OMS has always been able to expire one; what these tests cover
// is that something actually *runs* the sweep, on a clock rather than only
// when the next report happens to arrive -- an expiry mechanism nobody calls
// leaks working exposure forever, which is exactly the failure the in-flight
// risk accounting exists to prevent.

#include "hft/engine.hpp"
#include "test_harness.hpp"

using namespace hft;

namespace {

// An order the venue never answers: created through the OMS directly, so no
// execution report ever comes back for it.
ClOrdId send_and_abandon(Engine& engine, Nanos at_ns) {
  Order order;
  order.symbol = 0;
  order.side = Side::Buy;
  order.type = OrderType::Limit;
  order.price = 10000;
  order.quantity = 5;
  order.created_ts_ns = at_ns;
  return engine.oms().create(order, at_ns);
}

EngineConfig timeout_config() {
  EngineConfig cfg;
  cfg.threaded = false;
  cfg.oms.ack_timeout_ns = 1'000'000'000;  // 1s
  cfg.order_sweep_interval_ns = 100'000'000;  // 100ms
  return cfg;
}

}  // namespace

TEST(engine_expires_an_order_the_venue_never_answered) {
  Engine e(timeout_config());
  EngineStats stats;
  const Nanos t0 = 1'000'000'000;
  const ClOrdId id = send_and_abandon(e, t0);
  CHECK(id != 0);
  CHECK_EQ(e.oms().working_exposure(0), std::int64_t(5));

  // Well inside the ack timeout: nothing may be expired yet.
  CHECK_EQ(e.sweep_orders(t0 + 500'000'000, stats, /*force=*/true), std::size_t(0));
  CHECK_EQ(int(e.oms().find(id)->state), int(OrderState::PendingNew));

  // Past it.
  CHECK_EQ(e.sweep_orders(t0 + 1'500'000'000, stats, /*force=*/true), std::size_t(1));
  CHECK_EQ(int(e.oms().find(id)->state), int(OrderState::Expired));
  CHECK_EQ(stats.timed_out_orders, std::uint64_t(1));
  // The exposure the order was holding has to be released, or every later risk
  // check is measured against a quantity that will never trade.
  CHECK_EQ(e.oms().working_exposure(0), std::int64_t(0));
}

TEST(engine_halts_when_an_order_times_out) {
  Engine e(timeout_config());
  EngineStats stats;
  const Nanos t0 = 1'000'000'000;
  send_and_abandon(e, t0);
  CHECK_FALSE(e.risk().halted());

  e.sweep_orders(t0 + 2'000'000'000, stats, /*force=*/true);
  CHECK(e.risk().halted());
  CHECK(stats.halted);
  CHECK_EQ(int(e.risk().halt_reason()), int(HaltReason::OrderTimeout));
}

TEST(engine_can_be_configured_not_to_halt_on_a_timeout) {
  EngineConfig cfg = timeout_config();
  cfg.halt_on_order_timeout = false;
  Engine e(cfg);
  EngineStats stats;
  const Nanos t0 = 1'000'000'000;
  send_and_abandon(e, t0);

  CHECK_EQ(e.sweep_orders(t0 + 2'000'000'000, stats, /*force=*/true), std::size_t(1));
  // Still counted and still expired -- the option changes what we do about it,
  // never whether we noticed.
  CHECK_EQ(stats.timed_out_orders, std::uint64_t(1));
  CHECK_FALSE(e.risk().halted());
}

TEST(engine_rate_limits_the_sweep_between_intervals) {
  EngineConfig cfg = timeout_config();
  // Sweep interval deliberately longer than the ack timeout, so that "overdue"
  // and "swept" can be told apart.
  cfg.order_sweep_interval_ns = 5'000'000'000;
  Engine e(cfg);
  EngineStats stats;
  const Nanos t0 = 1'000'000'000;
  const ClOrdId id = send_and_abandon(e, t0);

  // First unforced call only starts the clock: an order sent in the first
  // microsecond of a run must not be judged against a timeout that began at
  // the epoch.
  CHECK_EQ(e.sweep_orders(t0, stats), std::size_t(0));
  // Overdue by the ack timeout, but inside the sweep interval.
  CHECK_EQ(e.sweep_orders(t0 + 2'000'000'000, stats), std::size_t(0));
  CHECK_EQ(int(e.oms().find(id)->state), int(OrderState::PendingNew));
  // Past the sweep interval.
  CHECK_EQ(e.sweep_orders(t0 + 6'000'000'000, stats), std::size_t(1));
  CHECK_EQ(int(e.oms().find(id)->state), int(OrderState::Expired));
}

TEST(engine_sweep_is_a_no_op_when_nothing_is_outstanding) {
  Engine e(timeout_config());
  EngineStats stats;
  CHECK_EQ(e.sweep_orders(5'000'000'000, stats, /*force=*/true), std::size_t(0));
  CHECK_EQ(stats.timed_out_orders, std::uint64_t(0));
  CHECK_FALSE(e.risk().halted());
}

TEST(engine_run_sweeps_without_being_asked) {
  // The paper venue answers synchronously, so a normal run leaves nothing
  // outstanding -- but an order abandoned before the run starts must still be
  // expired by the loop itself, with no explicit sweep call anywhere.
  EngineConfig cfg = timeout_config();
  cfg.oms.ack_timeout_ns = 1;  // anything unanswered is immediately overdue
  Engine e(cfg);
  send_and_abandon(e, 0);

  SyntheticFeed::Params p;
  p.total_events = 2000;
  p.seed = 7;
  SyntheticFeed feed(p);
  const EngineStats stats = e.run(feed);

  CHECK(stats.timed_out_orders >= 1);
  CHECK_EQ(e.oms().working_exposure(0), std::int64_t(0));
}
