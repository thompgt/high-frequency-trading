// Order-management tests.
//
// The state machine is the part of the engine that has to be right when the
// venue misbehaves, so most of what is below is about malformed, duplicated,
// late and racing reports rather than the happy path.

#include <vector>

#include "hft/engine.hpp"
#include "hft/feed.hpp"
#include "hft/oms.hpp"
#include "hft/risk.hpp"
#include "test_harness.hpp"

using namespace hft;

namespace {

Order make_order(SymbolId sym, Side side, Price px, Quantity qty) {
  Order o{};
  o.symbol = sym;
  o.side = side;
  o.type = OrderType::Limit;
  o.price = px;
  o.quantity = qty;
  return o;
}

ExecutionReport ack(ClOrdId id, Nanos ts = 100) {
  ExecutionReport r{};
  r.cl_ord_id = id;
  r.type = ExecType::Acked;
  r.venue_order_id = 900 + id;
  r.ts_ns = ts;
  return r;
}

ExecutionReport fill(ClOrdId id, Price px, Quantity qty, double fee = 0.0, Nanos ts = 200) {
  ExecutionReport r{};
  r.cl_ord_id = id;
  r.type = ExecType::Fill;
  r.price = px;
  r.quantity = qty;
  r.fee = fee;
  r.ts_ns = ts;
  return r;
}

ExecutionReport simple(ClOrdId id, ExecType t, Nanos ts = 300) {
  ExecutionReport r{};
  r.cl_ord_id = id;
  r.type = t;
  r.ts_ns = ts;
  return r;
}

}  // namespace

// ============================================================== happy path

TEST(oms_new_order_starts_pending_and_is_working) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  CHECK_NE(id, ClOrdId(0));

  const OrderRecord* rec = oms.find(id);
  CHECK(rec != nullptr);
  CHECK_EQ(int(rec->state), int(OrderState::PendingNew));
  CHECK_EQ(rec->leaves, Quantity(100));
  CHECK_EQ(rec->filled, Quantity(0));
  CHECK_EQ(oms.open_count(), std::size_t(1));
  // An unacknowledged order is still exposure.
  CHECK_EQ(oms.working_quantity(0, Side::Buy), Quantity(100));
  CHECK_EQ(oms.working_exposure(0), std::int64_t(100));
}

TEST(oms_ack_then_full_fill_completes_the_order) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  CHECK(oms.apply(ack(id)));
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::New));
  CHECK_EQ(oms.find(id)->venue_order_id, OrderId(900 + id));

  CHECK(oms.apply(fill(id, 10000, 100, 1.5)));
  const OrderRecord* rec = oms.find(id);
  CHECK_EQ(int(rec->state), int(OrderState::Filled));
  CHECK_EQ(rec->filled, Quantity(100));
  CHECK_EQ(rec->leaves, Quantity(0));
  CHECK_NEAR(rec->avg_fill_price, 100.0, 1e-9);
  CHECK_NEAR(rec->fees, 1.5, 1e-9);

  // Terminal orders stop counting as exposure and stop being open.
  CHECK_EQ(oms.open_count(), std::size_t(0));
  CHECK_EQ(oms.working_quantity(0, Side::Buy), Quantity(0));
  CHECK_EQ(oms.stats().filled_orders, std::uint64_t(1));
  CHECK_EQ(oms.stats().breaks(), std::uint64_t(0));
}

TEST(oms_partial_fills_accumulate_and_average) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  oms.apply(ack(id));

  CHECK(oms.apply(fill(id, 10000, 40)));  // $100.00 x 40
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::PartiallyFilled));
  CHECK_EQ(oms.find(id)->leaves, Quantity(60));
  CHECK_EQ(oms.working_quantity(0, Side::Buy), Quantity(60));

  CHECK(oms.apply(fill(id, 10100, 60)));  // $101.00 x 60
  const OrderRecord* rec = oms.find(id);
  CHECK_EQ(int(rec->state), int(OrderState::Filled));
  // (100.00*40 + 101.00*60) / 100
  CHECK_NEAR(rec->avg_fill_price, 100.6, 1e-9);
  CHECK_EQ(oms.stats().fills, std::uint64_t(2));
}

TEST(oms_tracks_exposure_per_symbol_and_side) {
  OrderManager oms;
  oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  oms.create(make_order(0, Side::Sell, 10100, 30), 10);
  oms.create(make_order(1, Side::Sell, 5000, 70), 10);

  CHECK_EQ(oms.working_quantity(0, Side::Buy), Quantity(100));
  CHECK_EQ(oms.working_quantity(0, Side::Sell), Quantity(30));
  CHECK_EQ(oms.working_exposure(0), std::int64_t(70));   // 100 long - 30 short
  CHECK_EQ(oms.working_exposure(1), std::int64_t(-70));
  CHECK_EQ(oms.gross_working_exposure(), std::int64_t(140));

  // A symbol we have never traded has no exposure and does not allocate.
  CHECK_EQ(oms.working_exposure(99), std::int64_t(0));
}

// ============================================================ cancellation

TEST(oms_cancel_request_moves_to_pending_cancel_then_cancelled) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  oms.apply(ack(id));

  CHECK(oms.request_cancel(id, 50));
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::PendingCancel));
  // Still working: a cancel that has not been confirmed has not cancelled
  // anything, and the order can still fill.
  CHECK_EQ(oms.working_quantity(0, Side::Buy), Quantity(100));

  CHECK(oms.apply(simple(id, ExecType::Cancelled)));
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::Cancelled));
  CHECK_EQ(oms.working_quantity(0, Side::Buy), Quantity(0));
  CHECK_EQ(oms.open_count(), std::size_t(0));
}

TEST(oms_pending_new_order_cannot_be_cancelled) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  // The venue has not confirmed the order exists, so there is nothing to
  // cancel yet.
  CHECK_FALSE(oms.request_cancel(id, 50));
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::PendingNew));
}

TEST(oms_cancel_reject_returns_the_order_to_live) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  oms.apply(ack(id));
  oms.request_cancel(id, 50);

  CHECK(oms.apply(simple(id, ExecType::CancelRejected)));
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::New));
  CHECK_EQ(oms.working_quantity(0, Side::Buy), Quantity(100));
  CHECK_EQ(oms.stats().cancel_rejected, std::uint64_t(1));
}

TEST(oms_cancel_reject_on_a_partly_filled_order_returns_to_partially_filled) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  oms.apply(ack(id));
  oms.apply(fill(id, 10000, 25));
  oms.request_cancel(id, 50);

  CHECK(oms.apply(simple(id, ExecType::CancelRejected)));
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::PartiallyFilled));
}

TEST(oms_fill_racing_a_cancel_is_accepted) {
  // The classic race: we send a cancel, the venue fills the order before it
  // reads the cancel. Rejecting the fill here would leave us short a position
  // we actually own.
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  oms.apply(ack(id));
  oms.request_cancel(id, 50);

  CHECK(oms.apply(fill(id, 10000, 40)));
  // Still PendingCancel: the cancel is outstanding on the remaining 60.
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::PendingCancel));
  CHECK_EQ(oms.find(id)->filled, Quantity(40));

  CHECK(oms.apply(simple(id, ExecType::Cancelled)));
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::Cancelled));
  CHECK_EQ(oms.working_quantity(0, Side::Buy), Quantity(0));
  CHECK_EQ(oms.stats().breaks(), std::uint64_t(0));
}

TEST(oms_accepts_an_unsolicited_cancel) {
  // Cancel-on-disconnect and end-of-day purges arrive without us asking.
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  oms.apply(ack(id));

  CHECK(oms.apply(simple(id, ExecType::Cancelled)));
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::Cancelled));
  CHECK_EQ(oms.stats().invalid_transitions, std::uint64_t(0));
}

// ============================================================ fill before ack

TEST(oms_fill_before_the_ack_is_accepted) {
  // Some venues send the first fill before (or instead of) the ack.
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  CHECK(oms.apply(fill(id, 10000, 100)));
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::Filled));
  CHECK_EQ(oms.stats().invalid_transitions, std::uint64_t(0));
}

// ================================================================= rejects

TEST(oms_venue_reject_is_terminal_and_frees_exposure) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  CHECK(oms.apply(simple(id, ExecType::VenueRejected)));
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::Rejected));
  CHECK_EQ(oms.working_quantity(0, Side::Buy), Quantity(0));
  CHECK_EQ(oms.open_count(), std::size_t(0));
  CHECK_EQ(oms.stats().venue_rejected, std::uint64_t(1));
}

TEST(oms_reject_after_an_ack_is_an_invalid_transition) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  oms.apply(ack(id));
  CHECK_FALSE(oms.apply(simple(id, ExecType::VenueRejected)));
  CHECK_EQ(int(oms.find(id)->state), int(OrderState::New));
  CHECK_EQ(oms.stats().invalid_transitions, std::uint64_t(1));
  CHECK(oms.stats().breaks() > 0);
}

TEST(oms_zero_quantity_order_is_refused) {
  OrderManager oms;
  CHECK_EQ(oms.create(make_order(0, Side::Buy, 10000, 0), 10), ClOrdId(0));
  CHECK_EQ(oms.create(make_order(0, Side::Buy, 10000, -5), 10), ClOrdId(0));
  CHECK_EQ(oms.open_count(), std::size_t(0));
}

// ============================================== reconciliation break paths

TEST(oms_report_for_an_unknown_id_is_counted_not_applied) {
  OrderManager oms;
  oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  CHECK_FALSE(oms.apply(ack(9999)));
  CHECK_EQ(oms.stats().unknown_reports, std::uint64_t(1));
  CHECK_EQ(oms.stats().late_reports, std::uint64_t(0));
}

TEST(oms_duplicate_ack_is_an_invalid_transition) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  CHECK(oms.apply(ack(id)));
  CHECK_FALSE(oms.apply(ack(id)));
  CHECK_EQ(oms.stats().acked, std::uint64_t(1));
  CHECK_EQ(oms.stats().invalid_transitions, std::uint64_t(1));
}

TEST(oms_fill_on_a_terminal_order_is_rejected) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  oms.apply(ack(id));
  oms.apply(fill(id, 10000, 100));
  CHECK_FALSE(oms.apply(fill(id, 10000, 10)));
  CHECK_EQ(oms.find(id)->filled, Quantity(100));
  CHECK_EQ(oms.stats().invalid_transitions, std::uint64_t(1));
}

TEST(oms_zero_or_negative_fill_quantity_is_rejected) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  oms.apply(ack(id));
  CHECK_FALSE(oms.apply(fill(id, 10000, 0)));
  CHECK_FALSE(oms.apply(fill(id, 10000, -10)));
  CHECK_EQ(oms.find(id)->filled, Quantity(0));
  CHECK_EQ(oms.stats().invalid_transitions, std::uint64_t(2));
}

TEST(oms_overfill_is_recorded_in_full_and_flagged) {
  // If the venue says it filled 150 on a 100-lot order then we own 150,
  // whatever our arithmetic thinks. Recording only 100 would leave us
  // believing we are flatter than we are, which is the dangerous direction.
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  oms.apply(ack(id));

  CHECK(oms.apply(fill(id, 10000, 150)));
  const OrderRecord* rec = oms.find(id);
  CHECK_EQ(rec->filled, Quantity(150));
  CHECK_EQ(rec->leaves, Quantity(0));
  CHECK_EQ(int(rec->state), int(OrderState::Filled));
  CHECK_EQ(oms.stats().overfills, std::uint64_t(1));
  CHECK(oms.stats().breaks() > 0);
  // Exposure must not go negative just because the venue overshot.
  CHECK_EQ(oms.working_quantity(0, Side::Buy), Quantity(0));
}

TEST(oms_late_report_is_distinguished_from_an_unknown_one) {
  OrderManager::Config cfg;
  cfg.max_open_orders = 4;
  cfg.retired_history = 2;  // remembers only the two most recent finished orders
  OrderManager oms(cfg);

  std::vector<ClOrdId> ids;
  for (int i = 0; i < 4; ++i) {
    const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 10), 10);
    oms.apply(ack(id));
    oms.apply(fill(id, 10000, 10));
    ids.push_back(id);
  }

  // The oldest two have fallen out of the history window.
  CHECK(oms.find(ids[0]) == nullptr);
  CHECK(oms.find(ids[3]) != nullptr);

  CHECK_FALSE(oms.apply(fill(ids[0], 10000, 1)));
  CHECK_EQ(oms.stats().late_reports, std::uint64_t(1));
  CHECK_EQ(oms.stats().unknown_reports, std::uint64_t(0));

  // An id we never issued at all is a different, more serious thing.
  CHECK_FALSE(oms.apply(fill(12345, 10000, 1)));
  CHECK_EQ(oms.stats().unknown_reports, std::uint64_t(1));
}

// ================================================================ capacity

TEST(oms_refuses_to_track_more_than_max_open_orders) {
  OrderManager::Config cfg;
  cfg.max_open_orders = 3;
  OrderManager oms(cfg);

  CHECK_NE(oms.create(make_order(0, Side::Buy, 10000, 1), 10), ClOrdId(0));
  CHECK_NE(oms.create(make_order(0, Side::Buy, 10000, 1), 10), ClOrdId(0));
  const ClOrdId third = oms.create(make_order(0, Side::Buy, 10000, 1), 10);
  CHECK_NE(third, ClOrdId(0));

  // Fourth is refused: an order we cannot track is an order we cannot cancel.
  CHECK_EQ(oms.create(make_order(0, Side::Buy, 10000, 1), 10), ClOrdId(0));
  CHECK_EQ(oms.stats().capacity_rejects, std::uint64_t(1));

  // Completing one frees the slot again.
  oms.apply(ack(third));
  oms.apply(fill(third, 10000, 1));
  CHECK_NE(oms.create(make_order(0, Side::Buy, 10000, 1), 10), ClOrdId(0));
}

TEST(oms_keeps_trading_when_history_is_full) {
  // A full history window must never stop us opening a new order.
  OrderManager::Config cfg;
  cfg.max_open_orders = 2;
  cfg.retired_history = 2;
  OrderManager oms(cfg);

  for (int i = 0; i < 50; ++i) {
    const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 1), 10);
    CHECK_NE(id, ClOrdId(0));
    oms.apply(ack(id));
    oms.apply(fill(id, 10000, 1));
  }
  CHECK_EQ(oms.stats().capacity_rejects, std::uint64_t(0));
  CHECK_EQ(oms.stats().filled_orders, std::uint64_t(50));
  CHECK_EQ(oms.open_count(), std::size_t(0));
}

// ================================================================ timeouts

TEST(oms_expires_orders_that_are_never_acknowledged) {
  OrderManager::Config cfg;
  cfg.ack_timeout_ns = 1000;
  OrderManager oms(cfg);

  const ClOrdId slow = oms.create(make_order(0, Side::Buy, 10000, 100), 0);
  const ClOrdId fast = oms.create(make_order(0, Side::Buy, 10000, 100), 0);
  oms.apply(ack(fast));

  std::vector<ClOrdId> expired;
  CHECK_EQ(oms.sweep_timeouts(500, &expired), std::size_t(0));  // not yet

  CHECK_EQ(oms.sweep_timeouts(1500, &expired), std::size_t(1));
  CHECK_EQ(expired.size(), std::size_t(1));
  CHECK_EQ(expired[0], slow);
  CHECK_EQ(int(oms.find(slow)->state), int(OrderState::Expired));
  // The acknowledged order is live and must not be swept.
  CHECK_EQ(int(oms.find(fast)->state), int(OrderState::New));

  // Expiry releases the exposure of the order whose fate we no longer know.
  CHECK_EQ(oms.working_quantity(0, Side::Buy), Quantity(100));
  CHECK_EQ(oms.stats().expired, std::uint64_t(1));
}

TEST(oms_sweep_is_idempotent) {
  OrderManager::Config cfg;
  cfg.ack_timeout_ns = 1000;
  OrderManager oms(cfg);
  oms.create(make_order(0, Side::Buy, 10000, 100), 0);

  CHECK_EQ(oms.sweep_timeouts(5000, nullptr), std::size_t(1));
  CHECK_EQ(oms.sweep_timeouts(9000, nullptr), std::size_t(0));
  CHECK_EQ(oms.stats().expired, std::uint64_t(1));
}

TEST(oms_a_fill_after_expiry_is_flagged_rather_than_silently_applied) {
  // Expired means "we do not know" -- so if the venue later says it filled,
  // that is a break someone has to look at, not a routine update.
  OrderManager::Config cfg;
  cfg.ack_timeout_ns = 1000;
  OrderManager oms(cfg);
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 0);
  oms.sweep_timeouts(5000, nullptr);

  CHECK_FALSE(oms.apply(fill(id, 10000, 100)));
  CHECK_EQ(oms.stats().invalid_transitions, std::uint64_t(1));
  CHECK(oms.stats().breaks() > 0);
}

// ============================================================ bookkeeping

TEST(oms_open_orders_are_listed_in_a_deterministic_order) {
  OrderManager oms;
  const ClOrdId a = oms.create(make_order(0, Side::Buy, 10000, 1), 10);
  const ClOrdId b = oms.create(make_order(1, Side::Sell, 10000, 1), 10);
  const ClOrdId c = oms.create(make_order(2, Side::Buy, 10000, 1), 10);
  oms.apply(ack(b));
  oms.apply(fill(b, 10000, 1));  // b completes, so drops out

  const std::vector<ClOrdId> open = oms.open_orders();
  CHECK_EQ(open.size(), std::size_t(2));
  CHECK_EQ(open[0], a);
  CHECK_EQ(open[1], c);
}

TEST(oms_clear_resets_everything) {
  OrderManager oms;
  const ClOrdId id = oms.create(make_order(0, Side::Buy, 10000, 100), 10);
  oms.apply(ack(id));
  oms.clear();

  CHECK_EQ(oms.open_count(), std::size_t(0));
  CHECK_EQ(oms.gross_working_exposure(), std::int64_t(0));
  CHECK_EQ(oms.stats().created, std::uint64_t(0));
  CHECK(oms.find(id) == nullptr);
  // Ids restart, so the manager is genuinely fresh.
  CHECK_EQ(oms.create(make_order(0, Side::Buy, 10000, 1), 10), ClOrdId(1));
}

TEST(oms_summary_reports_breaks) {
  OrderManager oms;
  oms.apply(ack(4242));  // unknown id
  const std::string s = oms.summary();
  CHECK(s.find("reconciliation breaks") != std::string::npos);
  CHECK(s.find("working orders") != std::string::npos);
}

TEST(oms_exposure_stays_consistent_through_a_randomised_lifecycle) {
  // Property check: at every point, the exposure counters must equal the sum
  // of `leaves` over working orders. Maintained incrementally, so drift here
  // would be invisible to the targeted tests above.
  OrderManager::Config cfg;
  cfg.max_open_orders = 64;
  cfg.retired_history = 64;
  OrderManager oms(cfg);

  std::uint64_t rng = 0xC0FFEEULL;
  auto next = [&rng]() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };

  std::vector<ClOrdId> live;
  for (int step = 0; step < 4000; ++step) {
    const int action = static_cast<int>(next() % 4);
    if (action == 0 || live.empty()) {
      const SymbolId sym = static_cast<SymbolId>(next() % 3);
      const Side side = (next() % 2) ? Side::Buy : Side::Sell;
      const Quantity qty = static_cast<Quantity>(1 + next() % 100);
      const ClOrdId id = oms.create(make_order(sym, side, 10000, qty), step);
      if (id != 0) live.push_back(id);
    } else {
      const std::size_t pick = next() % live.size();
      const ClOrdId id = live[pick];
      const OrderRecord* rec = oms.find(id);
      if (rec == nullptr || is_terminal(rec->state)) {
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(pick));
        continue;
      }
      if (action == 1) {
        // Only ack what is actually awaiting one -- a duplicate ack is a break
        // by design, and this test asserts there are none.
        if (rec->state == OrderState::PendingNew) oms.apply(ack(id));
      } else if (action == 2) {
        const Quantity q = static_cast<Quantity>(1 + next() % static_cast<std::uint64_t>(rec->leaves));
        oms.apply(fill(id, 10000, q));
      } else {
        if (oms.request_cancel(id, step)) oms.apply(simple(id, ExecType::Cancelled));
      }
    }

    // Recompute exposure from scratch and compare.
    std::int64_t expected_gross = 0;
    for (SymbolId sym = 0; sym < 3; ++sym) {
      Quantity buy = 0;
      Quantity sell = 0;
      for (const ClOrdId id : oms.open_orders()) {
        const OrderRecord* rec = oms.find(id);
        if (rec == nullptr || rec->symbol != sym) continue;
        (rec->side == Side::Buy ? buy : sell) += rec->leaves;
      }
      CHECK_EQ(oms.working_quantity(sym, Side::Buy), buy);
      CHECK_EQ(oms.working_quantity(sym, Side::Sell), sell);
      expected_gross += (buy - sell) < 0 ? (sell - buy) : (buy - sell);
    }
    CHECK_EQ(oms.gross_working_exposure(), expected_gross);
  }

  CHECK_EQ(oms.stats().overfills, std::uint64_t(0));
  CHECK_EQ(oms.stats().unknown_reports, std::uint64_t(0));
  CHECK_EQ(oms.stats().invalid_transitions, std::uint64_t(0));
}

// ================================================ in-flight risk integration

namespace {

// Stands in for the OMS so the risk checks can be driven to exact values.
class FakeExposure : public ExposureSource {
 public:
  std::int64_t net = 0;
  std::int64_t gross = 0;
  std::int64_t working_exposure(SymbolId) const override { return net; }
  std::int64_t gross_working_exposure() const override { return gross; }
};

Order risk_order(Side side, Quantity qty) {
  Order o{};
  o.symbol = 0;
  o.side = side;
  o.type = OrderType::Market;
  o.price = 10000;
  o.quantity = qty;
  return o;
}

}  // namespace

TEST(risk_position_limit_counts_orders_that_are_still_in_flight) {
  RiskLimits limits;
  limits.max_position_per_symbol = 100;
  limits.max_gross_position = 1'000'000;
  RiskManager risk(limits);

  // With no in-flight view, a 100-lot order is fine from a flat position...
  CHECK(risk.check(risk_order(Side::Buy, 100), 10000, 0).accepted);

  // ...but once 100 lots are already sent and unanswered, the same order would
  // put us at 200. This is the hole the exposure source closes: without it,
  // filled position is still zero and the check would pass.
  FakeExposure pending;
  pending.net = 100;
  pending.gross = 100;
  risk.set_exposure_source(&pending);

  const RiskDecision d = risk.check(risk_order(Side::Buy, 100), 10000, 0);
  CHECK_FALSE(d.accepted);
  CHECK_EQ(int(d.reason), int(RejectReason::PositionLimitExceeded));

  // An order that reduces the in-flight exposure is still allowed.
  CHECK(risk.check(risk_order(Side::Sell, 100), 10000, 0).accepted);
}

TEST(risk_gross_limit_counts_in_flight_exposure) {
  RiskLimits limits;
  limits.max_position_per_symbol = 1'000'000;
  limits.max_gross_position = 150;
  RiskManager risk(limits);

  FakeExposure pending;
  pending.net = 0;      // this symbol is flat in flight...
  pending.gross = 140;  // ...but other symbols carry 140 lots
  risk.set_exposure_source(&pending);

  CHECK(risk.check(risk_order(Side::Buy, 10), 10000, 0).accepted);
  const RiskDecision d = risk.check(risk_order(Side::Buy, 20), 10000, 0);
  CHECK_FALSE(d.accepted);
  CHECK_EQ(int(d.reason), int(RejectReason::GrossPositionLimitExceeded));
}

TEST(risk_without_an_exposure_source_behaves_as_before) {
  // Position-only checking must stay the default, so nothing that constructs a
  // bare RiskManager changes behaviour.
  RiskLimits limits;
  limits.max_position_per_symbol = 100;
  RiskManager risk(limits);
  CHECK(risk.exposure_source() == nullptr);
  CHECK(risk.check(risk_order(Side::Buy, 100), 10000, 0).accepted);
}

// ====================================================== engine integration

namespace {

EngineConfig oms_engine_config() {
  EngineConfig cfg;
  cfg.threaded = false;
  cfg.fast_window = 3;
  cfg.slow_window = 8;
  cfg.order_quantity = 5;
  cfg.record_curve = false;
  cfg.risk.max_orders_per_second = 1'000'000'000u;
  return cfg;
}

SyntheticFeed::Params oms_feed_params(std::size_t events) {
  SyntheticFeed::Params p;
  p.total_events = events;
  p.seed = 0xABCDEF01ULL;
  return p;
}

}  // namespace

TEST(engine_tracks_every_order_it_sends_and_leaves_none_working) {
  Engine engine(oms_engine_config());
  SyntheticFeed feed(oms_feed_params(120'000));
  EngineStats stats = engine.run(feed);
  engine.flatten(stats);

  const OmsStats& oms = engine.oms().stats();
  CHECK(stats.orders_sent > 0);
  // Every order the engine sent got an id and a lifecycle.
  CHECK_EQ(oms.created, stats.orders_sent + stats.flatten_orders);
  CHECK_EQ(oms.acked, oms.created);

  // Nothing is left in an unresolved state, and no exposure is stranded.
  CHECK_EQ(engine.oms().open_count(), std::size_t(0));
  CHECK_EQ(engine.oms().gross_working_exposure(), std::int64_t(0));

  // A synchronous venue should produce no reconciliation breaks at all. Any
  // count here means the engine and the OMS disagree about what happened.
  CHECK_EQ(oms.breaks(), std::uint64_t(0));
  CHECK_EQ(oms.unknown_reports, std::uint64_t(0));
  CHECK_EQ(oms.overfills, std::uint64_t(0));
}

TEST(engine_closes_out_the_remainder_of_a_partially_filled_order) {
  // Crossing a thin book fills part of a market order; the rest never rests.
  // If the engine left those remainders working, exposure would ratchet up
  // forever and the position limit would eventually reject everything.
  EngineConfig cfg = oms_engine_config();
  cfg.cross_book = true;
  cfg.order_quantity = 400;  // large enough to outrun the depth available
  Engine engine(cfg);

  SyntheticFeed feed(oms_feed_params(120'000));
  EngineStats stats = engine.run(feed);

  CHECK(stats.orders_sent > 0);
  CHECK_EQ(engine.oms().open_count(), std::size_t(0));
  CHECK_EQ(engine.oms().gross_working_exposure(), std::int64_t(0));
  CHECK_EQ(engine.oms().stats().breaks(), std::uint64_t(0));
}

TEST(engine_order_ids_line_up_with_the_venue_position) {
  // The OMS's own view of what filled must agree with the venue's position.
  // These are computed by completely separate code paths, so agreement is a
  // real check rather than a tautology.
  Engine engine(oms_engine_config());
  SyntheticFeed feed(oms_feed_params(80'000));
  EngineStats stats = engine.run(feed);

  std::int64_t oms_position = 0;
  for (ClOrdId id = 1; id <= engine.oms().stats().created; ++id) {
    const OrderRecord* rec = engine.oms().find(id);
    if (rec == nullptr) continue;  // aged out of the history window
    oms_position += (rec->side == Side::Buy ? rec->filled : -rec->filled);
  }

  // Only meaningful if the whole run still fits in the history window.
  if (engine.oms().stats().created <= engine.oms().config().retired_history) {
    CHECK_EQ(oms_position, engine.venue().position(0));
  }
  CHECK_EQ(stats.untracked_rejects, std::uint64_t(0));
}
