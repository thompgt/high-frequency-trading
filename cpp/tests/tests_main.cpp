// Unit tests for the HFT engine. Run with `make test`.
//
// The order book gets the most coverage by a wide margin, including a
// randomised differential test against an independently-maintained shadow
// model -- matching logic is the one place where a subtle bug silently
// corrupts everything downstream.

#include <cstdio>
#include <csignal>
#include <fstream>
#include <map>
#include <numeric>
#include <thread>
#include <type_traits>
#include <vector>

#include "hft/engine.hpp"
#include "hft/execution.hpp"
#include "hft/feed.hpp"
#include "hft/latency.hpp"
#include "hft/config.hpp"
#include "hft/log.hpp"
#include "hft/metrics.hpp"
#include "hft/order_book.hpp"
#include "hft/ring_buffer.hpp"
#include "hft/risk.hpp"
#include "hft/strategy.hpp"
#include "test_harness.hpp"

using namespace hft;

namespace {
OrderBook make_book() { return OrderBook(9000, 11000); }
}  // namespace

// =============================================================== LevelBitmap

TEST(bitmap_empty_reports_npos) {
  LevelBitmap m(1000);
  CHECK(m.empty());
  CHECK(m.find_highest() == LevelBitmap::npos);
  CHECK(m.find_lowest() == LevelBitmap::npos);
}

TEST(bitmap_tracks_extremes_across_words) {
  LevelBitmap m(5000);
  m.set(3);
  m.set(4095);
  m.set(1000);
  CHECK(m.test(3));
  CHECK(m.test(4095));
  CHECK_FALSE(m.test(7));
  CHECK_EQ(m.find_lowest(), std::size_t(3));
  CHECK_EQ(m.find_highest(), std::size_t(4095));

  m.clear(4095);
  CHECK_EQ(m.find_highest(), std::size_t(1000));
  m.clear(3);
  CHECK_EQ(m.find_lowest(), std::size_t(1000));
  m.clear(1000);
  CHECK(m.empty());
}

TEST(bitmap_rejects_oversized_band) {
  CHECK_THROWS(LevelBitmap(300000));
}

// ================================================================= OrderBook

TEST(book_starts_empty) {
  OrderBook b = make_book();
  CHECK_EQ(b.best_bid(), kNoPrice);
  CHECK_EQ(b.best_ask(), kNoPrice);
  CHECK_EQ(b.live_order_count(), std::size_t(0));
  double mid;
  CHECK_FALSE(b.mid_price(mid));
  Price sp;
  CHECK_FALSE(b.spread(sp));
}

TEST(book_rests_non_crossing_orders) {
  OrderBook b = make_book();
  CHECK_EQ(b.add_limit(1, Side::Buy, 9990, 100), Quantity(0));
  CHECK_EQ(b.add_limit(2, Side::Sell, 10010, 50), Quantity(0));
  CHECK_EQ(b.best_bid(), Price(9990));
  CHECK_EQ(b.best_ask(), Price(10010));
  CHECK_EQ(b.best_bid_quantity(), Quantity(100));
  CHECK_EQ(b.best_ask_quantity(), Quantity(50));
  CHECK_EQ(b.live_order_count(), std::size_t(2));

  Price sp;
  CHECK(b.spread(sp));
  CHECK_EQ(sp, Price(20));
  double mid;
  CHECK(b.mid_price(mid));
  CHECK_NEAR(mid, 100.0, 1e-9);
}

TEST(book_best_bid_is_highest_and_best_ask_is_lowest) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Buy, 9980, 10);
  b.add_limit(2, Side::Buy, 9995, 10);
  b.add_limit(3, Side::Buy, 9990, 10);
  b.add_limit(4, Side::Sell, 10050, 10);
  b.add_limit(5, Side::Sell, 10005, 10);
  b.add_limit(6, Side::Sell, 10020, 10);
  CHECK_EQ(b.best_bid(), Price(9995));
  CHECK_EQ(b.best_ask(), Price(10005));
}

TEST(book_aggregates_quantity_at_a_price_level) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Buy, 9990, 100);
  b.add_limit(2, Side::Buy, 9990, 250);
  b.add_limit(3, Side::Buy, 9990, 7);
  CHECK_EQ(b.quantity_at(Side::Buy, 9990), Quantity(357));
  CHECK_EQ(b.quantity_at(Side::Buy, 9991), Quantity(0));
  const auto d = b.depth(Side::Buy, 5);
  CHECK_EQ(d.size(), std::size_t(1));
  CHECK_EQ(d[0].order_count, std::uint32_t(3));
}

TEST(book_matches_time_priority_within_a_price_level) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Buy, 10000, 10);  // oldest -- must fill first
  b.add_limit(2, Side::Buy, 10000, 10);
  b.add_limit(3, Side::Buy, 10000, 10);

  std::vector<Trade> trades;
  const Quantity filled = b.add_limit(99, Side::Sell, 10000, 15, &trades);
  CHECK_EQ(filled, Quantity(15));
  CHECK_EQ(trades.size(), std::size_t(2));
  CHECK_EQ(trades[0].resting_id, OrderId(1));
  CHECK_EQ(trades[0].quantity, Quantity(10));
  CHECK_EQ(trades[1].resting_id, OrderId(2));
  CHECK_EQ(trades[1].quantity, Quantity(5));

  CHECK_FALSE(b.has_order(1));   // fully filled, gone
  CHECK(b.has_order(2));         // partially filled, still resting
  CHECK_EQ(b.quantity_at(Side::Buy, 10000), Quantity(15));
  CHECK_EQ(b.live_order_count(), std::size_t(2));
}

TEST(book_matches_price_priority_across_levels) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Sell, 10030, 10);
  b.add_limit(2, Side::Sell, 10010, 10);
  b.add_limit(3, Side::Sell, 10020, 10);

  std::vector<Trade> trades;
  // A buy at 10030 sweeps the cheapest liquidity first.
  const Quantity filled = b.add_limit(99, Side::Buy, 10030, 25, &trades);
  CHECK_EQ(filled, Quantity(25));
  CHECK_EQ(trades.size(), std::size_t(3));
  CHECK_EQ(trades[0].price, Price(10010));
  CHECK_EQ(trades[1].price, Price(10020));
  CHECK_EQ(trades[2].price, Price(10030));
  CHECK_EQ(trades[2].quantity, Quantity(5));
  CHECK_EQ(b.best_ask(), Price(10030));
  CHECK_EQ(b.best_ask_quantity(), Quantity(5));
}

TEST(book_gives_price_improvement_to_the_aggressor) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Sell, 10000, 10);
  std::vector<Trade> trades;
  // Buyer is willing to pay 10050 but the resting ask is 10000: the trade
  // prints at the resting price, and the buyer keeps the improvement.
  b.add_limit(2, Side::Buy, 10050, 10, &trades);
  CHECK_EQ(trades.size(), std::size_t(1));
  CHECK_EQ(trades[0].price, Price(10000));
  CHECK(trades[0].aggressor_side == Side::Buy);
}

TEST(book_does_not_cross_beyond_the_limit_price) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Sell, 10020, 10);
  std::vector<Trade> trades;
  const Quantity filled = b.add_limit(2, Side::Buy, 10010, 10, &trades);
  CHECK_EQ(filled, Quantity(0));
  CHECK_EQ(trades.size(), std::size_t(0));
  CHECK_EQ(b.best_bid(), Price(10010));
  CHECK_EQ(b.best_ask(), Price(10020));
}

TEST(book_rests_unfilled_remainder_of_a_crossing_order) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Sell, 10000, 4);
  std::vector<Trade> trades;
  const Quantity filled = b.add_limit(2, Side::Buy, 10000, 10, &trades);
  CHECK_EQ(filled, Quantity(4));
  CHECK_EQ(b.best_ask(), kNoPrice);          // ask side fully consumed
  CHECK_EQ(b.best_bid(), Price(10000));      // remainder rests as a bid
  CHECK_EQ(b.best_bid_quantity(), Quantity(6));
}

TEST(book_never_ends_up_crossed) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Buy, 10000, 10);
  b.add_limit(2, Side::Sell, 9990, 25);  // aggressive sell through the bid
  const Price bb = b.best_bid();
  const Price ba = b.best_ask();
  CHECK_EQ(bb, kNoPrice);
  CHECK_EQ(ba, Price(9990));
  CHECK_EQ(b.best_ask_quantity(), Quantity(15));
}

TEST(book_cancel_removes_the_order_and_updates_the_touch) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Buy, 9990, 10);
  b.add_limit(2, Side::Buy, 9995, 10);
  CHECK(b.cancel(2));
  CHECK_EQ(b.best_bid(), Price(9990));
  CHECK_EQ(b.live_order_count(), std::size_t(1));
  CHECK_FALSE(b.cancel(2));   // already gone
  CHECK_FALSE(b.cancel(999)); // never existed
}

TEST(book_cancel_of_a_fully_filled_order_is_rejected) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Buy, 10000, 10);
  b.execute_market(2, Side::Sell, 10);
  CHECK_FALSE(b.cancel(1));
  CHECK_EQ(b.live_order_count(), std::size_t(0));
}

TEST(book_rejects_invalid_orders) {
  OrderBook b = make_book();
  CHECK_EQ(b.add_limit(1, Side::Buy, 8000, 10), Quantity(-1));   // below band
  CHECK_EQ(b.add_limit(1, Side::Buy, 12000, 10), Quantity(-1));  // above band
  CHECK_EQ(b.add_limit(1, Side::Buy, 10000, 0), Quantity(-1));   // zero qty
  CHECK_EQ(b.add_limit(1, Side::Buy, 10000, -5), Quantity(-1));  // negative qty
  CHECK_EQ(b.add_limit(0, Side::Buy, 10000, 5), Quantity(-1));   // id 0 reserved
  CHECK_EQ(b.live_order_count(), std::size_t(0));

  CHECK_EQ(b.add_limit(7, Side::Buy, 10000, 5), Quantity(0));
  CHECK_EQ(b.add_limit(7, Side::Buy, 10001, 5), Quantity(-1));   // duplicate live id
  CHECK_EQ(b.live_order_count(), std::size_t(1));
}

TEST(book_modify_down_in_size_keeps_queue_priority) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Buy, 10000, 100);
  b.add_limit(2, Side::Buy, 10000, 100);
  CHECK(b.modify(1, 10000, 40));  // reduce, same price
  CHECK_EQ(b.quantity_at(Side::Buy, 10000), Quantity(140));

  std::vector<Trade> trades;
  b.add_limit(9, Side::Sell, 10000, 50, &trades);
  // Order 1 kept its place at the front of the queue despite the amend.
  CHECK_EQ(trades[0].resting_id, OrderId(1));
  CHECK_EQ(trades[0].quantity, Quantity(40));
  CHECK_EQ(trades[1].resting_id, OrderId(2));
}

TEST(book_modify_up_in_size_loses_queue_priority) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Buy, 10000, 100);
  b.add_limit(2, Side::Buy, 10000, 100);
  CHECK(b.modify(1, 10000, 150));  // size increase -> cancel/replace
  CHECK_EQ(b.quantity_at(Side::Buy, 10000), Quantity(250));

  std::vector<Trade> trades;
  b.add_limit(9, Side::Sell, 10000, 10, &trades);
  CHECK_EQ(trades[0].resting_id, OrderId(2));  // order 2 is now first in queue
}

TEST(book_modify_can_reprice_into_a_cross) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Sell, 10020, 10);
  b.add_limit(2, Side::Buy, 10000, 10);
  std::vector<Trade> trades;
  CHECK(b.modify(2, 10020, 10, &trades));  // lift the offer via an amend
  CHECK_EQ(trades.size(), std::size_t(1));
  CHECK_EQ(trades[0].price, Price(10020));
  CHECK_EQ(b.live_order_count(), std::size_t(0));
}

TEST(book_modify_to_zero_cancels_and_out_of_band_is_rejected) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Buy, 10000, 10);
  CHECK_FALSE(b.modify(1, 50000, 10));  // out of band: order must survive
  CHECK(b.has_order(1));
  CHECK(b.modify(1, 10000, 0));
  CHECK_FALSE(b.has_order(1));
  CHECK_FALSE(b.modify(1, 10000, 5));  // no longer exists
}

TEST(book_market_order_sweeps_until_filled_or_book_is_empty) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Sell, 10000, 5);
  b.add_limit(2, Side::Sell, 10010, 5);
  std::vector<Trade> trades;
  const Quantity filled = b.execute_market(9, Side::Buy, 100, &trades);
  CHECK_EQ(filled, Quantity(10));  // unfilled remainder is dropped, not rested
  CHECK_EQ(trades.size(), std::size_t(2));
  CHECK_EQ(b.best_ask(), kNoPrice);
  CHECK_EQ(b.live_order_count(), std::size_t(0));

  CHECK_EQ(b.execute_market(10, Side::Buy, 100), Quantity(0));  // empty book
}

TEST(book_depth_walks_levels_in_priority_order) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Buy, 9950, 1);
  b.add_limit(2, Side::Buy, 9990, 2);
  b.add_limit(3, Side::Buy, 9970, 3);
  b.add_limit(4, Side::Sell, 10100, 4);
  b.add_limit(5, Side::Sell, 10010, 5);
  b.add_limit(6, Side::Sell, 10050, 6);

  const auto bids = b.depth(Side::Buy, 10);
  CHECK_EQ(bids.size(), std::size_t(3));
  CHECK_EQ(bids[0].price, Price(9990));
  CHECK_EQ(bids[1].price, Price(9970));
  CHECK_EQ(bids[2].price, Price(9950));

  const auto asks = b.depth(Side::Sell, 2);
  CHECK_EQ(asks.size(), std::size_t(2));
  CHECK_EQ(asks[0].price, Price(10010));
  CHECK_EQ(asks[1].price, Price(10050));
  CHECK_EQ(asks[0].quantity, Quantity(5));
}

TEST(book_clear_resets_everything) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Buy, 9990, 10);
  b.add_limit(2, Side::Sell, 10010, 10);
  b.clear();
  CHECK_EQ(b.best_bid(), kNoPrice);
  CHECK_EQ(b.best_ask(), kNoPrice);
  CHECK_EQ(b.live_order_count(), std::size_t(0));
  CHECK_EQ(b.add_limit(1, Side::Buy, 9990, 10), Quantity(0));  // id reusable
}

TEST(book_survives_heavy_reuse_of_the_order_id_table) {
  // Exercises the open-addressed id index's tombstone handling and rehash:
  // 50k inserts and erases through a table that starts at 1024 slots.
  OrderBook b = make_book();
  for (OrderId i = 1; i <= 50000; ++i) {
    CHECK_EQ(b.add_limit(i, Side::Buy, 9990, 1), Quantity(0));
    CHECK(b.cancel(i));
  }
  CHECK_EQ(b.live_order_count(), std::size_t(0));
  CHECK_EQ(b.best_bid(), kNoPrice);
}

// Randomised differential test: drive the book with pseudo-random order flow
// and check it against a shadow model maintained independently from the trade
// reports. Any divergence in quantity, order count or the touch fails here.
TEST(book_matches_an_independent_shadow_model_under_random_flow) {
  OrderBook b = make_book();

  struct Rec {
    Side side;
    Price price;
    Quantity qty;
  };
  std::map<OrderId, Rec> shadow;

  std::uint64_t s = 0xC0FFEE123ULL;
  auto rnd = [&]() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1DULL;
  };

  std::vector<OrderId> ids;
  std::vector<Trade> trades;
  OrderId next = 1;

  auto consume = [&](const std::vector<Trade>& ts) {
    for (const auto& t : ts) {
      auto it = shadow.find(t.resting_id);
      CHECK(it != shadow.end());
      it->second.qty -= t.quantity;
      CHECK(it->second.qty >= 0);
      if (it->second.qty == 0) shadow.erase(it);
    }
  };

  for (int step = 0; step < 40000; ++step) {
    const int roll = static_cast<int>(rnd() % 100);
    trades.clear();

    if (roll < 60 || shadow.empty()) {
      const Side side = (rnd() & 1) ? Side::Buy : Side::Sell;
      const Price price = 9900 + static_cast<Price>(rnd() % 200);
      const Quantity qty = 1 + static_cast<Quantity>(rnd() % 50);
      const OrderId id = next++;
      const Quantity filled = b.add_limit(id, side, price, qty, &trades);
      CHECK(filled >= 0);
      consume(trades);
      Quantity traded = 0;
      for (const auto& t : trades) traded += t.quantity;
      CHECK_EQ(traded, filled);
      if (qty - filled > 0) {
        shadow[id] = Rec{side, price, qty - filled};
        ids.push_back(id);
      }
    } else if (roll < 85) {
      const OrderId id = ids[static_cast<std::size_t>(rnd() % ids.size())];
      const bool expected = shadow.count(id) != 0;
      CHECK_EQ(b.cancel(id), expected);
      shadow.erase(id);
    } else {
      const Side side = (rnd() & 1) ? Side::Buy : Side::Sell;
      const Quantity qty = 1 + static_cast<Quantity>(rnd() % 300);
      const Quantity filled = b.execute_market(next++, side, qty, &trades);
      consume(trades);
      Quantity traded = 0;
      for (const auto& t : trades) traded += t.quantity;
      CHECK_EQ(traded, filled);
    }

    // The book must never be crossed after any operation.
    const Price bb = b.best_bid();
    const Price ba = b.best_ask();
    if (bb != kNoPrice && ba != kNoPrice) CHECK(bb < ba);
  }

  // Full reconciliation against the shadow model.
  CHECK_EQ(b.live_order_count(), shadow.size());
  std::map<std::pair<int, Price>, Quantity> expected;
  Price shadow_best_bid = kNoPrice;
  Price shadow_best_ask = kNoPrice;
  for (const auto& kv : shadow) {
    expected[{static_cast<int>(kv.second.side), kv.second.price}] += kv.second.qty;
    if (kv.second.side == Side::Buy) {
      if (shadow_best_bid == kNoPrice || kv.second.price > shadow_best_bid) {
        shadow_best_bid = kv.second.price;
      }
    } else {
      if (shadow_best_ask == kNoPrice || kv.second.price < shadow_best_ask) {
        shadow_best_ask = kv.second.price;
      }
    }
  }
  for (const auto& kv : expected) {
    const Side side = static_cast<Side>(kv.first.first);
    CHECK_EQ(b.quantity_at(side, kv.first.second), kv.second);
  }
  CHECK_EQ(b.best_bid(), shadow_best_bid);
  CHECK_EQ(b.best_ask(), shadow_best_ask);
}

// ============================================================ SpscRingBuffer

TEST(ring_rounds_capacity_up_to_a_power_of_two) {
  SpscRingBuffer<Tick> r(1000);
  CHECK_EQ(r.capacity(), std::size_t(1024));
  SpscRingBuffer<Tick> r2(1);
  CHECK_EQ(r2.capacity(), std::size_t(2));
}

TEST(ring_preserves_fifo_order) {
  SpscRingBuffer<Tick> r(8);
  Tick t{};
  for (int i = 0; i < 5; ++i) {
    t.price = i;
    CHECK(r.try_push(t));
  }
  CHECK_EQ(r.size_approx(), std::size_t(5));
  for (int i = 0; i < 5; ++i) {
    Tick got{};
    CHECK(r.try_pop(got));
    CHECK_EQ(got.price, Price(i));
  }
  Tick got{};
  CHECK_FALSE(r.try_pop(got));
}

TEST(ring_drops_the_newest_item_when_full) {
  SpscRingBuffer<Tick> r(4);
  Tick t{};
  for (int i = 0; i < 4; ++i) {
    t.price = i;
    CHECK(r.push_or_drop(t));
  }
  t.price = 99;
  CHECK_FALSE(r.push_or_drop(t));
  CHECK_EQ(r.dropped(), std::uint64_t(1));

  Tick got{};
  CHECK(r.try_pop(got));
  CHECK_EQ(got.price, Price(0));  // the oldest survived
  CHECK(r.push_or_drop(t));       // and there is room again
}

TEST(ring_try_push_failure_is_not_counted_as_a_drop) {
  // A producer that spins until there is room has not lost a message; only
  // push_or_drop() records a genuine loss.
  SpscRingBuffer<Tick> r(2);
  Tick t{};
  CHECK(r.try_push(t));
  CHECK(r.try_push(t));
  CHECK_FALSE(r.try_push(t));
  CHECK_EQ(r.dropped(), std::uint64_t(0));
}

TEST(ring_wraps_around_indefinitely) {
  SpscRingBuffer<Tick> r(4);
  Tick t{};
  Tick got{};
  for (int i = 0; i < 1000; ++i) {
    t.price = i;
    CHECK(r.try_push(t));
    CHECK(r.try_pop(got));
    CHECK_EQ(got.price, Price(i));
  }
  CHECK_EQ(r.dropped(), std::uint64_t(0));
}

TEST(ring_hands_every_item_across_threads_intact) {
  // The real point of the lock-free design: two threads, no mutex, no losses,
  // no reordering, no torn reads.
  constexpr int kN = 500000;
  SpscRingBuffer<Tick> r(1024);
  std::int64_t consumed_sum = 0;
  int consumed = 0;

  std::thread producer([&] {
    Tick t{};
    for (int i = 0; i < kN; ++i) {
      t.price = i;
      t.quantity = i;  // second field: a torn read would break the invariant
      while (!r.try_push(t)) std::this_thread::yield();
    }
  });

  Tick got{};
  while (consumed < kN) {
    if (r.try_pop(got)) {
      CHECK_EQ(got.price, Price(consumed));  // strict FIFO
      CHECK_EQ(got.quantity, got.price);     // not torn
      consumed_sum += got.price;
      ++consumed;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();

  const std::int64_t expected = static_cast<std::int64_t>(kN) * (kN - 1) / 2;
  CHECK_EQ(consumed, kN);
  CHECK_EQ(consumed_sum, expected);
  CHECK_EQ(r.dropped(), std::uint64_t(0));
}

// ================================================================== Strategy

namespace {
Tick tick_at(Price p) {
  Tick t{};
  t.symbol = 0;
  t.price = p;
  t.type = TickType::Trade;
  return t;
}
}  // namespace

TEST(strategy_rejects_bad_windows) {
  CHECK_THROWS(MovingAverageCrossover(5, 5));
  CHECK_THROWS(MovingAverageCrossover(20, 5));
  CHECK_THROWS(MovingAverageCrossover(0, 5));
}

TEST(strategy_stays_silent_until_the_slow_window_is_full) {
  MovingAverageCrossover s(2, 5);
  Signal sig{};
  for (int i = 0; i < 4; ++i) CHECK_FALSE(s.on_tick(tick_at(10000 + i), 10000 + i, sig));
  double f, sl;
  CHECK_FALSE(s.averages(0, f, sl));
  // The 5th tick fills the window but only establishes the baseline state.
  CHECK_FALSE(s.on_tick(tick_at(10004), 10004, sig));
  CHECK(s.averages(0, f, sl));
}

TEST(strategy_fires_only_on_a_state_change) {
  MovingAverageCrossover s(2, 5);
  Signal sig{};
  // Rising prices: fast average sits above slow. Baseline established, no fire.
  for (Price p = 10000; p <= 10004; ++p) CHECK_FALSE(s.on_tick(tick_at(p), p, sig));
  CHECK_FALSE(s.on_tick(tick_at(10005), 10005, sig));  // still fast-above, no change
  CHECK_EQ(s.signals_emitted(), std::uint64_t(0));

  // Now drop hard so the fast average crosses below -> SELL.
  bool fired = false;
  for (Price p = 9990; p >= 9960 && !fired; p -= 5) fired = s.on_tick(tick_at(p), p, sig);
  CHECK(fired);
  CHECK(sig.side == Side::Sell);
  CHECK_EQ(s.signals_emitted(), std::uint64_t(1));

  // And back up -> BUY.
  fired = false;
  for (Price p = 9970; p <= 10100 && !fired; p += 10) fired = s.on_tick(tick_at(p), p, sig);
  CHECK(fired);
  CHECK(sig.side == Side::Buy);
  CHECK_EQ(s.signals_emitted(), std::uint64_t(2));
}

TEST(strategy_running_sums_equal_a_naive_recomputation) {
  // Guards the O(1) rolling-sum optimisation against the O(n) definition the
  // Python implementation used.
  constexpr std::size_t kFast = 5, kSlow = 20;
  MovingAverageCrossover s(kFast, kSlow);
  std::vector<Price> history;
  Signal sig{};
  std::uint64_t seed = 42;

  for (int i = 0; i < 5000; ++i) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    const Price p = 9900 + static_cast<Price>((seed >> 33) % 200);
    history.push_back(p);
    s.on_tick(tick_at(p), p, sig);

    if (history.size() < kSlow) continue;
    double fast_avg, slow_avg;
    CHECK(s.averages(0, fast_avg, slow_avg));

    std::int64_t naive_slow = 0, naive_fast = 0;
    for (std::size_t k = history.size() - kSlow; k < history.size(); ++k) naive_slow += history[k];
    for (std::size_t k = history.size() - kFast; k < history.size(); ++k) naive_fast += history[k];
    CHECK_NEAR(slow_avg, static_cast<double>(naive_slow) / kSlow, 1e-9);
    CHECK_NEAR(fast_avg, static_cast<double>(naive_fast) / kFast, 1e-9);
  }
}

TEST(strategy_keeps_symbols_independent) {
  MovingAverageCrossover s(2, 4);
  Signal sig{};
  for (Price p = 10000; p < 10004; ++p) {
    Tick a = tick_at(p);
    a.symbol = 1;
    Tick b = tick_at(20000 - p);
    b.symbol = 2;
    s.on_tick(a, p, sig);
    s.on_tick(b, 20000 - p, sig);
  }
  double f1, s1, f2, s2;
  CHECK(s.averages(1, f1, s1));
  CHECK(s.averages(2, f2, s2));
  CHECK(f1 > s1);   // symbol 1 rising
  CHECK(f2 < s2);   // symbol 2 falling
}

// ================================================================ PaperVenue

namespace {
Order mk_order(OrderId id, Side side, Quantity qty) {
  Order o{};
  o.id = id;
  o.symbol = 0;
  o.side = side;
  o.type = OrderType::Market;
  o.quantity = qty;
  return o;
}
}  // namespace

TEST(venue_applies_slippage_in_the_costly_direction) {
  PaperVenue v(PaperVenue::Config{100.0, 0.0, nullptr});  // 100bps = 1%
  const Fill buy = v.submit(mk_order(1, Side::Buy, 1), 10000);
  CHECK_EQ(buy.price, Price(10100));
  const Fill sell = v.submit(mk_order(2, Side::Sell, 1), 10000);
  CHECK_EQ(sell.price, Price(9900));
}

TEST(venue_realizes_pnl_only_on_the_closing_portion) {
  PaperVenue v(PaperVenue::Config{0.0, 0.0, nullptr});
  v.submit(mk_order(1, Side::Buy, 10), 10000);   // long 10 @ $100
  CHECK_NEAR(v.realized_pnl(), 0.0, 1e-9);       // nothing closed yet
  CHECK_EQ(v.position(0), std::int64_t(10));

  v.submit(mk_order(2, Side::Sell, 4), 11000);   // sell 4 @ $110
  CHECK_NEAR(v.realized_pnl(), 40.0, 1e-9);      // 4 * $10
  CHECK_EQ(v.position(0), std::int64_t(6));
}

TEST(venue_averages_cost_basis_when_adding_to_a_position) {
  PaperVenue v(PaperVenue::Config{0.0, 0.0, nullptr});
  v.submit(mk_order(1, Side::Buy, 10), 10000);  // 10 @ $100
  v.submit(mk_order(2, Side::Buy, 10), 12000);  // 10 @ $120 -> avg $110
  v.submit(mk_order(3, Side::Sell, 20), 13000); // out at $130
  CHECK_NEAR(v.realized_pnl(), 20 * 20.0, 1e-9);
  CHECK_EQ(v.position(0), std::int64_t(0));
}

TEST(venue_handles_flipping_from_long_to_short) {
  PaperVenue v(PaperVenue::Config{0.0, 0.0, nullptr});
  v.submit(mk_order(1, Side::Buy, 10), 10000);   // long 10 @ $100
  v.submit(mk_order(2, Side::Sell, 25), 11000);  // sell 25 @ $110 -> short 15
  CHECK_EQ(v.position(0), std::int64_t(-15));
  CHECK_NEAR(v.realized_pnl(), 100.0, 1e-9);     // only the 10 long closed
  v.submit(mk_order(3, Side::Buy, 15), 10500);   // cover 15 @ $105
  CHECK_EQ(v.position(0), std::int64_t(0));
  CHECK_NEAR(v.realized_pnl(), 100.0 + 15 * 5.0, 1e-9);
}

TEST(venue_charges_fees_against_realized_pnl) {
  PaperVenue v(PaperVenue::Config{0.0, 10.0, nullptr});  // 10bps
  v.submit(mk_order(1, Side::Buy, 100), 10000);          // $10000 notional
  CHECK_NEAR(v.fees_paid(), 10.0, 1e-9);
  CHECK_NEAR(v.realized_pnl(), -10.0, 1e-9);
}

TEST(venue_marks_open_positions_to_market) {
  PaperVenue v(PaperVenue::Config{0.0, 0.0, nullptr});
  v.submit(mk_order(1, Side::Buy, 10), 10000);
  CHECK_NEAR(v.equity(0, 10000), 0.0, 1e-9);
  CHECK_NEAR(v.equity(0, 11000), 100.0, 1e-9);   // +$10 x 10
  CHECK_NEAR(v.equity(0, 9000), -100.0, 1e-9);
}

TEST(venue_crossing_the_book_pays_the_volume_weighted_price) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Sell, 10000, 5);
  b.add_limit(2, Side::Sell, 10100, 5);
  SingleBookProvider books(&b);
  PaperVenue v(PaperVenue::Config{0.0, 0.0, &books});
  const Fill f = v.submit(mk_order(9, Side::Buy, 10), 10000);
  CHECK_EQ(f.quantity, Quantity(10));
  CHECK_EQ(f.price, Price(10050));  // (5*10000 + 5*10100) / 10
  CHECK_EQ(b.live_order_count(), std::size_t(0));
}

TEST(venue_records_an_equity_curve) {
  PaperVenue v(PaperVenue::Config{0.0, 0.0, nullptr});
  v.submit(mk_order(1, Side::Buy, 10), 10000);
  v.submit(mk_order(2, Side::Sell, 10), 11000);
  const auto& curve = v.equity_curve();
  CHECK_EQ(curve.size(), std::size_t(2));
  CHECK_EQ(curve[0].fill_index, std::uint64_t(1));
  CHECK_NEAR(curve[1].realized_pnl, 100.0, 1e-9);
  CHECK_EQ(curve[1].position, std::int64_t(0));
}

// ========================================================= LatencyHistogram

TEST(histogram_buckets_contain_the_values_they_index) {
  for (Nanos v : {Nanos(0), Nanos(1), Nanos(31), Nanos(32), Nanos(33), Nanos(63), Nanos(64),
                  Nanos(1000), Nanos(999999), Nanos(1'000'000'000)}) {
    const std::size_t i = LatencyHistogram::bucket_index(v);
    CHECK(LatencyHistogram::bucket_low(i) <= v);
    CHECK(LatencyHistogram::bucket_high(i) >= v);
  }
}

TEST(histogram_bucket_indices_are_monotonic) {
  std::size_t prev = 0;
  for (Nanos v = 0; v < 100000; v = v + 1 + v / 8) {
    const std::size_t i = LatencyHistogram::bucket_index(v);
    CHECK(i >= prev);
    prev = i;
  }
}

TEST(histogram_reports_accurate_percentiles) {
  LatencyHistogram h;
  for (int i = 1; i <= 10000; ++i) h.record(i);
  CHECK_EQ(h.count(), std::uint64_t(10000));
  CHECK_EQ(h.min(), Nanos(1));
  CHECK_EQ(h.max(), Nanos(10000));
  CHECK_NEAR(h.mean(), 5000.5, 0.5);
  // Bucketed, so allow the documented ~3% resolution error.
  CHECK_NEAR(h.percentile(50.0), 5000.0, 5000.0 * 0.05);
  CHECK_NEAR(h.percentile(99.0), 9900.0, 9900.0 * 0.05);
  CHECK_NEAR(h.percentile(99.9), 9990.0, 9990.0 * 0.05);
  CHECK_EQ(h.percentile(100.0), Nanos(10000));
}

TEST(histogram_percentiles_are_non_decreasing) {
  LatencyHistogram h;
  std::uint64_t s = 7;
  for (int i = 0; i < 20000; ++i) {
    s = s * 6364136223846793005ULL + 1;
    h.record(static_cast<Nanos>((s >> 40) % 1000000));
  }
  Nanos prev = -1;
  for (double p : {1.0, 25.0, 50.0, 90.0, 99.0, 99.9, 100.0}) {
    const Nanos v = h.percentile(p);
    CHECK(v >= prev);
    prev = v;
  }
}

TEST(histogram_merges_and_resets) {
  LatencyHistogram a, b;
  for (int i = 0; i < 100; ++i) a.record(10);
  for (int i = 0; i < 100; ++i) b.record(1000);
  a.merge(b);
  CHECK_EQ(a.count(), std::uint64_t(200));
  CHECK_EQ(a.min(), Nanos(10));
  CHECK_EQ(a.max(), Nanos(1000));
  a.reset();
  CHECK_EQ(a.count(), std::uint64_t(0));
  CHECK_EQ(a.percentile(50.0), Nanos(0));
}

TEST(histogram_is_empty_safe) {
  LatencyHistogram h;
  CHECK_EQ(h.count(), std::uint64_t(0));
  CHECK_EQ(h.min(), Nanos(0));
  CHECK_EQ(h.max(), Nanos(0));
  CHECK_NEAR(h.mean(), 0.0, 1e-9);
}

// ====================================================================== Feed

TEST(feed_is_deterministic_for_a_given_seed) {
  SyntheticFeed::Params p;
  p.total_events = 5000;
  p.seed = 12345;
  SyntheticFeed a(p), b(p);
  Tick ta{}, tb{};
  int n = 0;
  while (a.next(ta)) {
    CHECK(b.next(tb));
    CHECK(ta.type == tb.type);
    CHECK(ta.side == tb.side);
    CHECK_EQ(ta.price, tb.price);
    CHECK_EQ(ta.quantity, tb.quantity);
    CHECK_EQ(ta.order_id, tb.order_id);
    ++n;
  }
  CHECK_FALSE(b.next(tb));
  CHECK_EQ(n, 5000);
}

TEST(feed_reset_replays_the_same_stream) {
  SyntheticFeed::Params p;
  p.total_events = 500;
  SyntheticFeed f(p);
  std::vector<Price> first;
  Tick t{};
  while (f.next(t)) first.push_back(t.price);
  f.reset();
  std::size_t i = 0;
  while (f.next(t)) {
    CHECK_EQ(t.price, first[i]);
    ++i;
  }
  CHECK_EQ(i, first.size());
}

TEST(feed_stays_inside_its_price_band) {
  SyntheticFeed::Params p;
  p.total_events = 200000;
  SyntheticFeed f(p);
  Tick t{};
  while (f.next(t)) {
    if (t.type == TickType::CancelOrder) continue;
    CHECK(t.price >= p.min_price);
    CHECK(t.price <= p.max_price);
    CHECK(t.quantity > 0);
  }
}

TEST(replay_csv_round_trips_through_the_file) {
  const std::string path = "build/test_replay.csv";
  SyntheticFeed::Params p;
  p.seed = 777;
  CHECK(SyntheticFeed::write_replay_csv(path, p, 1000));

  CsvReplayFeed r(path);
  CHECK(r.ok());
  CHECK_EQ(r.size(), std::size_t(1000));

  p.total_events = 1000;
  SyntheticFeed src(p);
  Tick expected{}, got{};
  while (src.next(expected)) {
    CHECK(r.next(got));
    CHECK(got.type == expected.type);
    CHECK_EQ(got.price, expected.price);
    CHECK_EQ(got.quantity, expected.quantity);
    CHECK_EQ(got.order_id, expected.order_id);
  }
  CHECK_FALSE(r.next(got));
}

TEST(replay_reports_a_missing_file_instead_of_crashing) {
  CsvReplayFeed r("build/definitely_not_here_12345.csv");
  CHECK_FALSE(r.ok());
  CHECK(!r.error().empty());
}

// ==================================================================== Engine

TEST(engine_runs_end_to_end_and_keeps_the_book_consistent) {
  EngineConfig cfg;
  cfg.threaded = false;
  cfg.record_curve = true;
  Engine e(cfg);

  SyntheticFeed::Params p;
  p.total_events = 200000;
  p.seed = 2024;
  SyntheticFeed feed(p);

  const EngineStats st = e.run(feed);
  CHECK_EQ(st.ticks, std::uint64_t(200000));
  CHECK(st.adds > 0);
  CHECK(st.cancels > 0);
  CHECK(st.book_trades > 0);
  CHECK(st.signals > 0);
  // Every signal is either sent or stopped by risk -- nothing is lost silently.
  CHECK_EQ(st.signals, st.orders_sent + st.risk_rejects);
  CHECK_EQ(st.dropped_ticks, std::uint64_t(0));

  const Price bb = e.book().best_bid();
  const Price ba = e.book().best_ask();
  if (bb != kNoPrice && ba != kNoPrice) CHECK(bb < ba);
  CHECK_EQ(e.venue().fill_count(), st.orders_sent);
  CHECK(e.latency().tick_to_order.count() == st.orders_sent);
  CHECK(e.latency().book_update.count() == st.ticks);
}

TEST(engine_threaded_and_inline_agree_on_results) {
  // The ring-buffer hand-off must not change what the engine computes -- only
  // where the work happens.
  auto run = [](bool threaded) {
    EngineConfig cfg;
    cfg.threaded = threaded;
    Engine e(cfg);
    SyntheticFeed::Params p;
    p.total_events = 100000;
    p.seed = 99;
    SyntheticFeed feed(p);
    const EngineStats st = e.run(feed);
    return std::make_tuple(st.ticks, st.signals, st.book_trades, e.venue().realized_pnl(),
                           e.book().live_order_count());
  };
  const auto a = run(false);
  const auto b = run(true);
  CHECK_EQ(std::get<0>(a), std::get<0>(b));
  CHECK_EQ(std::get<1>(a), std::get<1>(b));
  CHECK_EQ(std::get<2>(a), std::get<2>(b));
  CHECK_NEAR(std::get<3>(a), std::get<3>(b), 1e-6);
  CHECK_EQ(std::get<4>(a), std::get<4>(b));
}

TEST(engine_writes_the_csvs_the_notebook_consumes) {
  EngineConfig cfg;
  cfg.threaded = false;
  Engine e(cfg);
  SyntheticFeed::Params p;
  p.total_events = 50000;
  SyntheticFeed feed(p);
  e.run(feed);

  CHECK(e.latency().write_summary_csv("build/test_latency_summary.csv"));
  CHECK(e.latency().write_histogram_csv("build/test_latency_hist.csv"));
  CHECK(e.venue().write_fills_csv("build/test_fills.csv"));
  CHECK(e.write_book_snapshot_csv("build/test_book.csv", 10));
}


// =========================================== self-match prevention (order book)

TEST(book_without_stp_lets_an_owner_trade_against_itself) {
  OrderBook b = make_book();
  b.add_limit(1, Side::Sell, 10000, 10, nullptr, /*owner=*/7);
  std::vector<Trade> trades;
  b.add_limit(2, Side::Buy, 10000, 10, &trades, /*owner=*/7);
  CHECK_EQ(trades.size(), std::size_t(1));  // documented default: no STP
  CHECK_EQ(b.self_match_preventions(), std::uint64_t(0));
}

TEST(book_stp_cancel_resting_skips_own_order_and_trades_the_next) {
  OrderBook b = make_book();
  b.set_self_match_policy(SelfMatchPolicy::CancelResting);
  b.add_limit(1, Side::Sell, 10000, 10, nullptr, /*owner=*/7);   // ours
  b.add_limit(2, Side::Sell, 10000, 10, nullptr, /*owner=*/9);   // someone else's

  std::vector<Trade> trades;
  const Quantity filled = b.add_limit(3, Side::Buy, 10000, 10, &trades, /*owner=*/7);
  CHECK_EQ(filled, Quantity(10));
  CHECK_EQ(trades.size(), std::size_t(1));
  CHECK_EQ(trades[0].resting_id, OrderId(2));   // traded with the other party
  CHECK_FALSE(b.has_order(1));                  // our resting order was cancelled
  CHECK_EQ(b.self_match_preventions(), std::uint64_t(1));
  CHECK_EQ(b.live_order_count(), std::size_t(0));
}

TEST(book_stp_cancel_aggressor_stops_the_order_without_resting_it) {
  OrderBook b = make_book();
  b.set_self_match_policy(SelfMatchPolicy::CancelAggressor);
  b.add_limit(1, Side::Sell, 10000, 10, nullptr, /*owner=*/7);

  std::vector<Trade> trades;
  const Quantity filled = b.add_limit(2, Side::Buy, 10000, 25, &trades, /*owner=*/7);
  CHECK_EQ(filled, Quantity(0));
  CHECK_EQ(trades.size(), std::size_t(0));
  CHECK(b.has_order(1));            // resting order untouched
  CHECK_FALSE(b.has_order(2));      // aggressor remainder was NOT rested
  CHECK_EQ(b.self_match_preventions(), std::uint64_t(1));
  CHECK_EQ(b.best_bid(), kNoPrice);
}

TEST(book_stp_ignores_unowned_orders) {
  // owner 0 means "unspecified"; STP must not fire and merge unrelated flow.
  OrderBook b = make_book();
  b.set_self_match_policy(SelfMatchPolicy::CancelResting);
  b.add_limit(1, Side::Sell, 10000, 10);  // owner 0
  std::vector<Trade> trades;
  b.add_limit(2, Side::Buy, 10000, 10, &trades);  // owner 0
  CHECK_EQ(trades.size(), std::size_t(1));
  CHECK_EQ(b.self_match_preventions(), std::uint64_t(0));
}

TEST(book_stp_survives_partial_depth_exhaustion) {
  OrderBook b = make_book();
  b.set_self_match_policy(SelfMatchPolicy::CancelResting);
  b.add_limit(1, Side::Sell, 10000, 5, nullptr, 7);
  b.add_limit(2, Side::Sell, 10010, 5, nullptr, 7);
  std::vector<Trade> trades;
  // Every level belongs to us: the aggressor cancels its way through the whole
  // book and ends up resting with nothing traded.
  const Quantity filled = b.add_limit(3, Side::Buy, 10020, 8, &trades, 7);
  CHECK_EQ(filled, Quantity(0));
  CHECK_EQ(trades.size(), std::size_t(0));
  CHECK_EQ(b.self_match_preventions(), std::uint64_t(2));
  CHECK_EQ(b.best_ask(), kNoPrice);
  CHECK_EQ(b.best_bid(), Price(10020));  // remainder rests under CancelResting
}

// ================================================================ RiskManager

namespace {

RiskLimits permissive_limits() {
  RiskLimits l;
  l.max_order_quantity = 1000;
  l.max_order_notional = 1000000.0;
  l.max_position_per_symbol = 10000;
  l.max_gross_position = 20000;
  l.price_collar_bps = 500.0;
  l.max_orders_per_second = 1000000;
  l.max_daily_orders = 1000000;
  l.max_drawdown = 10000.0;
  return l;
}

Order risk_order(Side side, Quantity qty, Price price = 10000, SymbolId sym = 0) {
  Order o{};
  o.id = 1;
  o.symbol = sym;
  o.side = side;
  o.type = OrderType::Market;
  o.price = price;
  o.quantity = qty;
  return o;
}

Fill fill_of(const Order& o, Quantity qty) {
  Fill f{};
  f.symbol = o.symbol;
  f.side = o.side;
  f.quantity = qty;
  f.price = o.price;
  return f;
}

}  // namespace

TEST(risk_accepts_an_ordinary_order) {
  RiskManager r(permissive_limits());
  const auto d = r.check(risk_order(Side::Buy, 10), 10000, 0);
  CHECK(d.accepted);
  CHECK(d.reason == RejectReason::None);
  CHECK_EQ(r.accepted_orders(), std::uint64_t(1));
  CHECK_EQ(r.rejected_orders(), std::uint64_t(0));
}

TEST(risk_rejects_non_positive_quantity_even_when_disabled) {
  RiskLimits l = permissive_limits();
  l.enabled = false;
  RiskManager r(l);
  const auto d = r.check(risk_order(Side::Buy, 0), 10000, 0);
  CHECK_FALSE(d.accepted);
  CHECK(d.reason == RejectReason::InvalidOrder);
}

TEST(risk_rejects_fat_finger_quantity) {
  RiskLimits l = permissive_limits();
  l.max_order_quantity = 100;
  RiskManager r(l);
  CHECK(r.check(risk_order(Side::Buy, 100), 10000, 0).accepted);
  const auto d = r.check(risk_order(Side::Buy, 101), 10000, 0);
  CHECK_FALSE(d.accepted);
  CHECK(d.reason == RejectReason::OrderQuantityTooLarge);
  CHECK_EQ(r.rejects(RejectReason::OrderQuantityTooLarge), std::uint64_t(1));
}

TEST(risk_rejects_fat_finger_notional) {
  RiskLimits l = permissive_limits();
  l.max_order_notional = 50000.0;  // $50k
  RiskManager r(l);
  // 400 shares at $100 = $40k: fine. 600 = $60k: not.
  CHECK(r.check(risk_order(Side::Buy, 400, 10000), 10000, 0).accepted);
  const auto d = r.check(risk_order(Side::Buy, 600, 10000), 10000, 0);
  CHECK_FALSE(d.accepted);
  CHECK(d.reason == RejectReason::OrderNotionalTooLarge);
}

TEST(risk_rejects_prices_outside_the_collar) {
  RiskLimits l = permissive_limits();
  l.price_collar_bps = 100.0;  // 1%
  RiskManager r(l);
  CHECK(r.check(risk_order(Side::Buy, 1, 10090), 10000, 0).accepted);   // +0.9%
  const auto hi = r.check(risk_order(Side::Buy, 1, 10200), 10000, 0);   // +2%
  CHECK_FALSE(hi.accepted);
  CHECK(hi.reason == RejectReason::PriceOutsideCollar);
  const auto lo = r.check(risk_order(Side::Sell, 1, 9800), 10000, 0);   // -2%
  CHECK_FALSE(lo.accepted);
  CHECK(lo.reason == RejectReason::PriceOutsideCollar);
  CHECK_EQ(r.rejects(RejectReason::PriceOutsideCollar), std::uint64_t(2));
}

TEST(risk_enforces_per_symbol_position_limit) {
  RiskLimits l = permissive_limits();
  l.max_position_per_symbol = 100;
  RiskManager r(l);

  const Order buy = risk_order(Side::Buy, 100);
  CHECK(r.check(buy, 10000, 0).accepted);
  r.on_fill(fill_of(buy, 100));
  CHECK_EQ(r.position(0), std::int64_t(100));

  // Another buy would take us to 200: rejected.
  const auto d = r.check(risk_order(Side::Buy, 100), 10000, 0);
  CHECK_FALSE(d.accepted);
  CHECK(d.reason == RejectReason::PositionLimitExceeded);

  // But selling reduces exposure and must still be allowed.
  CHECK(r.check(risk_order(Side::Sell, 100), 10000, 0).accepted);
}

TEST(risk_position_limit_is_symmetric_for_shorts) {
  RiskLimits l = permissive_limits();
  l.max_position_per_symbol = 50;
  RiskManager r(l);
  const Order sell = risk_order(Side::Sell, 50);
  CHECK(r.check(sell, 10000, 0).accepted);
  r.on_fill(fill_of(sell, 50));
  CHECK_EQ(r.position(0), std::int64_t(-50));
  const auto d = r.check(risk_order(Side::Sell, 10), 10000, 0);
  CHECK_FALSE(d.accepted);
  CHECK(d.reason == RejectReason::PositionLimitExceeded);
}

TEST(risk_enforces_gross_position_across_symbols) {
  RiskLimits l = permissive_limits();
  l.max_position_per_symbol = 1000;
  l.max_gross_position = 150;
  RiskManager r(l);

  const Order a = risk_order(Side::Buy, 100, 10000, /*sym=*/0);
  CHECK(r.check(a, 10000, 0).accepted);
  r.on_fill(fill_of(a, 100));

  // A short in another symbol still adds to *gross* exposure.
  const auto d = r.check(risk_order(Side::Sell, 100, 10000, /*sym=*/1), 10000, 0);
  CHECK_FALSE(d.accepted);
  CHECK(d.reason == RejectReason::GrossPositionLimitExceeded);

  const Order small = risk_order(Side::Sell, 40, 10000, /*sym=*/1);
  CHECK(r.check(small, 10000, 0).accepted);
  r.on_fill(fill_of(small, 40));
  CHECK_EQ(r.gross_position(), std::int64_t(140));
}

TEST(risk_throttles_orders_per_second) {
  RiskLimits l = permissive_limits();
  l.max_orders_per_second = 3;
  RiskManager r(l);
  const Nanos t0 = 1000000000;

  for (int i = 0; i < 3; ++i) CHECK(r.check(risk_order(Side::Buy, 1), 10000, t0).accepted);
  const auto d = r.check(risk_order(Side::Buy, 1), 10000, t0);
  CHECK_FALSE(d.accepted);
  CHECK(d.reason == RejectReason::ThrottleExceeded);

  // A new one-second window resets the allowance.
  CHECK(r.check(risk_order(Side::Buy, 1), 10000, t0 + 1000000000).accepted);
}

TEST(risk_daily_order_limit_engages_the_kill_switch) {
  RiskLimits l = permissive_limits();
  l.max_daily_orders = 2;
  RiskManager r(l);
  CHECK(r.check(risk_order(Side::Buy, 1), 10000, 0).accepted);
  CHECK(r.check(risk_order(Side::Buy, 1), 10000, 0).accepted);

  const auto d = r.check(risk_order(Side::Buy, 1), 10000, 0);
  CHECK_FALSE(d.accepted);
  CHECK(d.reason == RejectReason::DailyOrderLimitExceeded);
  CHECK(r.halted());
  CHECK(r.halt_reason() == HaltReason::DailyOrderLimit);

  // Rolling the day clears the counter but deliberately NOT the kill switch.
  r.roll_day();
  CHECK(r.halted());
  const auto d2 = r.check(risk_order(Side::Buy, 1), 10000, 0);
  CHECK(d2.reason == RejectReason::KillSwitchEngaged);
  r.release_kill_switch();
  CHECK(r.check(risk_order(Side::Buy, 1), 10000, 0).accepted);
}

TEST(risk_drawdown_breach_trips_the_kill_switch_and_is_sticky) {
  RiskLimits l = permissive_limits();
  l.max_drawdown = 500.0;
  RiskManager r(l);

  r.on_equity(1000.0);   // peak
  r.on_equity(700.0);    // -300, within budget
  CHECK_FALSE(r.halted());
  CHECK_NEAR(r.drawdown(), 300.0, 1e-9);

  r.on_equity(400.0);    // -600, breach
  CHECK(r.halted());
  CHECK(r.halt_reason() == HaltReason::DrawdownBreach);
  CHECK(r.should_flatten());

  // Recovering does NOT un-halt: the day's loss budget is already spent.
  r.on_equity(1200.0);
  CHECK(r.halted());

  const auto d = r.check(risk_order(Side::Buy, 1), 10000, 0);
  CHECK_FALSE(d.accepted);
  CHECK(d.reason == RejectReason::KillSwitchEngaged);
}

TEST(risk_manual_kill_switch_blocks_everything) {
  RiskManager r(permissive_limits());
  CHECK_FALSE(r.halted());
  r.engage_kill_switch(HaltReason::Manual);
  CHECK(r.halted());
  const auto d = r.check(risk_order(Side::Buy, 1), 10000, 0);
  CHECK_FALSE(d.accepted);
  CHECK(d.reason == RejectReason::KillSwitchEngaged);
  CHECK_EQ(r.rejects(RejectReason::KillSwitchEngaged), std::uint64_t(1));

  r.release_kill_switch();
  CHECK_FALSE(r.halted());
  CHECK(r.check(risk_order(Side::Buy, 1), 10000, 0).accepted);
}

TEST(risk_first_halt_reason_wins) {
  RiskManager r(permissive_limits());
  r.engage_kill_switch(HaltReason::DrawdownBreach);
  r.engage_kill_switch(HaltReason::Manual);
  CHECK(r.halt_reason() == HaltReason::DrawdownBreach);
}

TEST(risk_disabled_limits_still_count_orders) {
  RiskLimits l = permissive_limits();
  l.enabled = false;
  l.max_order_quantity = 1;
  RiskManager r(l);
  CHECK(r.check(risk_order(Side::Buy, 1000000), 10000, 0).accepted);
  CHECK_EQ(r.accepted_orders(), std::uint64_t(1));
}

TEST(risk_rejects_when_no_usable_price_is_available) {
  RiskManager r(permissive_limits());
  const auto d = r.check(risk_order(Side::Buy, 1, /*price=*/0), /*reference=*/0, 0);
  CHECK_FALSE(d.accepted);
  CHECK(d.reason == RejectReason::InvalidOrder);
}

TEST(risk_summary_lists_reject_reasons) {
  RiskLimits l = permissive_limits();
  l.max_order_quantity = 1;
  RiskManager r(l);
  r.check(risk_order(Side::Buy, 99), 10000, 0);
  const std::string s = r.summary();
  CHECK(s.find("order_quantity_too_large") != std::string::npos);
}

// ================================================== engine <-> risk integration

TEST(engine_stops_sending_orders_once_a_limit_binds) {
  // The crossover strategy alternates buy/sell, so it never builds a large
  // position on its own. Bind the fat-finger size limit instead: it is
  // unconditional, so every order the strategy produces must be stopped.
  EngineConfig cfg;
  cfg.threaded = false;
  cfg.order_quantity = 10;
  cfg.risk.max_order_quantity = 5;         // smaller than every order we send
  cfg.risk.max_orders_per_second = 100'000'000;
  Engine e(cfg);

  SyntheticFeed::Params p;
  p.total_events = 200000;
  p.seed = 31337;
  SyntheticFeed feed(p);
  const EngineStats st = e.run(feed);

  CHECK(st.signals > 0);
  CHECK_EQ(st.orders_sent, std::uint64_t(0));       // nothing reached the venue
  CHECK_EQ(st.risk_rejects, st.signals);
  CHECK_EQ(e.venue().position(0), std::int64_t(0));
  CHECK_EQ(e.risk().rejects(RejectReason::OrderQuantityTooLarge), st.signals);
}

TEST(engine_throttle_caps_order_rate_over_a_compressed_run) {
  // A synthetic run replays far faster than real time, so a production-sane
  // 1000 orders/sec throttle binds hard. That is the throttle working, and the
  // engine must keep running rather than treating it as an error.
  EngineConfig cfg;
  cfg.threaded = false;
  cfg.order_quantity = 1;
  cfg.risk.max_orders_per_second = 100;
  Engine e(cfg);

  SyntheticFeed::Params p;
  p.total_events = 300000;
  p.seed = 777;
  SyntheticFeed feed(p);
  const EngineStats st = e.run(feed);

  CHECK(st.signals > 0);
  CHECK(e.risk().rejects(RejectReason::ThrottleExceeded) > 0);
  CHECK_EQ(st.signals, st.orders_sent + st.risk_rejects);
  CHECK_FALSE(e.risk().halted());  // a throttle is back-pressure, not a halt
}

TEST(engine_flatten_closes_out_inventory) {
  EngineConfig cfg;
  cfg.threaded = false;
  cfg.order_quantity = 10;
  Engine e(cfg);

  SyntheticFeed::Params p;
  p.total_events = 100000;
  p.seed = 4242;
  SyntheticFeed feed(p);
  EngineStats st = e.run(feed);

  if (e.venue().position(0) != 0) {
    const std::uint64_t sent = e.flatten(st);
    CHECK(sent > 0);
    CHECK_EQ(e.venue().position(0), std::int64_t(0));
    CHECK_EQ(e.risk().position(0), std::int64_t(0));
  }
}

TEST(engine_honours_a_stop_request) {
  EngineConfig cfg;
  cfg.threaded = false;
  Engine e(cfg);
  SyntheticFeed::Params p;
  p.total_events = 1000000;
  SyntheticFeed feed(p);
  e.request_stop();
  const EngineStats st = e.run(feed);
  CHECK_EQ(st.ticks, std::uint64_t(0));  // stopped before consuming anything
}


// ==================================================================== Config

namespace {

// Writes a temporary config file and returns its path.
std::string write_temp_config(const char* name, const std::string& body) {
  const std::string path = std::string("build/") + name;
  std::ofstream f(path);
  f << body;
  f.close();
  return path;
}

}  // namespace

TEST(config_defaults_are_valid) {
  AppConfig cfg;
  std::vector<ConfigError> errors;
  CHECK(validate_config(cfg, errors));
  CHECK_EQ(errors.size(), std::size_t(0));
}

TEST(config_parses_a_well_formed_file) {
  const std::string path = write_temp_config("cfg_ok.conf",
      "# a comment\n"
      "\n"
      "symbol = TEST   # trailing comment\n"
      "fast_window = 3\n"
      "slow_window = 9\n"
      "order_quantity = 25\n"
      "fee_bps = 1.25\n"
      "cross_book = false\n"
      "max_drawdown = 500\n"
      "log_level = warn\n");

  AppConfig cfg;
  std::vector<ConfigError> errors;
  CHECK(load_config_file(path, cfg, errors));
  CHECK_EQ(errors.size(), std::size_t(0));
  CHECK(cfg.symbol == "TEST");
  CHECK_EQ(cfg.engine.fast_window, std::size_t(3));
  CHECK_EQ(cfg.engine.slow_window, std::size_t(9));
  CHECK_EQ(cfg.engine.order_quantity, Quantity(25));
  CHECK_NEAR(cfg.engine.fee_bps, 1.25, 1e-9);
  CHECK_FALSE(cfg.engine.cross_book);
  CHECK_NEAR(cfg.engine.risk.max_drawdown, 500.0, 1e-9);
  CHECK(cfg.log_level == LogLevel::Warn);
}

TEST(config_rejects_an_unknown_key) {
  // The whole point: a typo must be loud, not silently ignored.
  const std::string path = write_temp_config("cfg_typo.conf", "max_postion_per_symbol = 10\n");
  AppConfig cfg;
  std::vector<ConfigError> errors;
  CHECK_FALSE(load_config_file(path, cfg, errors));
  CHECK_EQ(errors.size(), std::size_t(1));
  CHECK(errors[0].message.find("unknown setting") != std::string::npos);
  CHECK_EQ(errors[0].line, std::size_t(1));
}

TEST(config_rejects_malformed_values) {
  AppConfig cfg;
  std::string err;
  CHECK_FALSE(apply_config_setting("fast_window", "abc", cfg, err));
  CHECK_FALSE(err.empty());
  CHECK_FALSE(apply_config_setting("fast_window", "10abc", cfg, err));  // partial parse
  CHECK_FALSE(apply_config_setting("fast_window", "", cfg, err));
  CHECK_FALSE(apply_config_setting("fast_window", "0", cfg, err));      // must be positive
  CHECK_FALSE(apply_config_setting("fast_window", "-3", cfg, err));
  CHECK_FALSE(apply_config_setting("cross_book", "maybe", cfg, err));
  CHECK_FALSE(apply_config_setting("fee_bps", "-1", cfg, err));
  CHECK_FALSE(apply_config_setting("log_level", "verbose", cfg, err));
  CHECK_FALSE(apply_config_setting("symbol", "", cfg, err));
}

TEST(config_accepts_boolean_spellings) {
  AppConfig cfg;
  std::string err;
  for (const char* yes : {"1", "true", "TRUE", "yes", "on"}) {
    CHECK(apply_config_setting("threaded", yes, cfg, err));
    CHECK(cfg.engine.threaded);
  }
  for (const char* no : {"0", "false", "FALSE", "no", "off"}) {
    CHECK(apply_config_setting("threaded", no, cfg, err));
    CHECK_FALSE(cfg.engine.threaded);
  }
}

TEST(config_reports_a_missing_file) {
  AppConfig cfg;
  std::vector<ConfigError> errors;
  CHECK_FALSE(load_config_file("build/definitely_missing_config.conf", cfg, errors));
  CHECK_EQ(errors.size(), std::size_t(1));
  CHECK(errors[0].message.find("cannot open") != std::string::npos);
}

TEST(config_reports_a_line_without_an_equals_sign) {
  const std::string path = write_temp_config("cfg_bad_line.conf", "symbol = OK\njust some words\n");
  AppConfig cfg;
  std::vector<ConfigError> errors;
  CHECK_FALSE(load_config_file(path, cfg, errors));
  CHECK_EQ(errors[0].line, std::size_t(2));
}

TEST(config_validation_catches_contradictory_settings) {
  {
    AppConfig cfg;
    cfg.engine.fast_window = 20;
    cfg.engine.slow_window = 5;
    std::vector<ConfigError> errors;
    CHECK_FALSE(validate_config(cfg, errors));
  }
  {
    // order_quantity above max_order_quantity would reject every single order.
    AppConfig cfg;
    cfg.engine.order_quantity = 100;
    cfg.engine.risk.max_order_quantity = 10;
    std::vector<ConfigError> errors;
    CHECK_FALSE(validate_config(cfg, errors));
  }
  {
    // A per-symbol limit above the gross limit means gross can never bind.
    AppConfig cfg;
    cfg.engine.risk.max_position_per_symbol = 500;
    cfg.engine.risk.max_gross_position = 100;
    std::vector<ConfigError> errors;
    CHECK_FALSE(validate_config(cfg, errors));
  }
  {
    AppConfig cfg;
    cfg.engine.min_price = 1;
    cfg.engine.max_price = 300000;  // wider than the book can address
    std::vector<ConfigError> errors;
    CHECK_FALSE(validate_config(cfg, errors));
  }
  {
    AppConfig cfg;
    cfg.events = 0;
    std::vector<ConfigError> errors;
    CHECK_FALSE(validate_config(cfg, errors));
  }
}

TEST(config_shipped_example_file_loads_and_validates) {
  // Guards against the documented example drifting away from the parser.
  AppConfig cfg;
  std::vector<ConfigError> errors;
  if (load_config_file("config/engine.conf", cfg, errors)) {
    CHECK(validate_config(cfg, errors));
    CHECK_EQ(errors.size(), std::size_t(0));
  } else {
    // Running from a different working directory: only fail on real parse
    // errors, not on the file being absent.
    CHECK(errors.size() == 1 && errors[0].message.find("cannot open") != std::string::npos);
  }
}

TEST(config_describe_mentions_every_key) {
  AppConfig cfg;
  const std::string text = describe_config(cfg);
  for (const auto& key : config_keys()) {
    CHECK(text.find(key) != std::string::npos);
  }
}

// ==================================================================== Logger

TEST(log_level_parsing_round_trips) {
  LogLevel l;
  CHECK(parse_log_level("info", l));
  CHECK(l == LogLevel::Info);
  CHECK(parse_log_level("WARN", l));
  CHECK(l == LogLevel::Warn);
  CHECK(parse_log_level("warning", l));
  CHECK(l == LogLevel::Warn);
  CHECK(parse_log_level("off", l));
  CHECK(l == LogLevel::Off);
  CHECK_FALSE(parse_log_level("chatty", l));
}

TEST(log_filters_below_the_configured_level) {
  Logger lg(LogLevel::Warn, stdout);
  CHECK_FALSE(lg.enabled(LogLevel::Info));
  CHECK_FALSE(lg.enabled(LogLevel::Debug));
  CHECK(lg.enabled(LogLevel::Warn));
  CHECK(lg.enabled(LogLevel::Error));

  lg.set_level(LogLevel::Off);
  CHECK_FALSE(lg.enabled(LogLevel::Error));  // Off silences everything
}

TEST(log_writes_records_asynchronously_and_flushes_on_stop) {
  std::FILE* f = std::fopen("build/test_log.txt", "w");
  CHECK(f != nullptr);
  {
    Logger lg(LogLevel::Info, f, 1024);
    lg.start();
    for (int i = 0; i < 500; ++i) {
      lg.log(LogLevel::Info, "tick processed", "seq", static_cast<double>(i));
    }
    lg.log(LogLevel::Warn, "three fields", "a", 1, "b", 2.5, "c", 3);
    lg.stop();  // must drain everything still queued
    CHECK_EQ(lg.written(), std::uint64_t(501));
  }
  std::fclose(f);

  std::ifstream in("build/test_log.txt");
  CHECK(in.good());
  std::string line;
  int lines = 0;
  bool saw_fields = false;
  while (std::getline(in, line)) {
    ++lines;
    if (line.find("a=1 b=2.5000 c=3") != std::string::npos) saw_fields = true;
  }
  CHECK_EQ(lines, 501);
  CHECK(saw_fields);
}

TEST(log_drops_rather_than_blocking_when_the_queue_is_full) {
  // Correct behaviour under back-pressure: lose a log line, never delay an
  // order. The loss must be counted so it is not silent.
  std::FILE* devnull = std::fopen("build/test_log_drop.txt", "w");
  CHECK(devnull != nullptr);
  Logger lg(LogLevel::Info, devnull, 4);
  // Deliberately NOT started: with no writer draining, the tiny queue fills.
  // (emit() writes synchronously when stopped, so start then immediately
  // saturate instead.)
  lg.start();
  for (int i = 0; i < 200000; ++i) lg.log(LogLevel::Info, "flood", "i", static_cast<double>(i));
  lg.stop();
  std::fclose(devnull);
  // Either everything got written or some were dropped -- but the two must
  // always account for every record submitted.
  CHECK_EQ(lg.written() + lg.dropped(), std::uint64_t(200000));
}

TEST(log_record_is_trivially_copyable) {
  // Required for the SPSC ring: a record must be publishable with a plain
  // store, with no constructor running on the consumer side.
  CHECK(std::is_trivially_copyable<LogRecord>::value);
}

// =================================================================== Metrics

TEST(metrics_json_reports_the_whole_run) {
  EngineConfig cfg;
  cfg.threaded = false;
  cfg.order_quantity = 5;
  cfg.risk.max_orders_per_second = 100'000'000;
  Engine e(cfg);

  SymbolTable symbols;
  const SymbolId sym = symbols.intern("TESTSYM");
  SyntheticFeed::Params p;
  p.symbol = sym;
  p.total_events = 100000;
  p.seed = 5150;
  SyntheticFeed feed(p);
  const EngineStats st = e.run(feed);

  const std::string json = metrics_json(e, st, symbols);
  for (const char* needle : {"\"schema_version\"", "\"run\"", "\"book\"", "\"trading\"",
                             "\"risk\"", "\"latency\"", "\"rejects_by_reason\"",
                             "\"tick_to_order\"", "\"p999_ns\"", "TESTSYM",
                             "position_limit_exceeded", "\"halt_reason\""}) {
    CHECK(json.find(needle) != std::string::npos);
  }
  // Balanced braces is a cheap structural sanity check on hand-rolled JSON.
  int depth = 0;
  bool in_string = false;
  for (std::size_t i = 0; i < json.size(); ++i) {
    const char c = json[i];
    if (c == '"' && (i == 0 || json[i - 1] != '\\')) in_string = !in_string;
    if (in_string) continue;
    if (c == '{') ++depth;
    if (c == '}') --depth;
    CHECK(depth >= 0);
  }
  CHECK_EQ(depth, 0);

  CHECK(write_metrics_json("build/test_metrics.json", e, st, symbols));
}

TEST(metrics_json_escapes_symbol_names) {
  EngineConfig cfg;
  cfg.threaded = false;
  Engine e(cfg);
  SymbolTable symbols;
  symbols.intern("we\"ird");
  EngineStats st;
  const std::string json = metrics_json(e, st, symbols);
  CHECK(json.find("we\\\"ird") != std::string::npos || json.find("positions\": {}") != std::string::npos);
}

TEST(metrics_write_reports_failure_on_a_bad_path) {
  EngineConfig cfg;
  cfg.threaded = false;
  Engine e(cfg);
  SymbolTable symbols;
  EngineStats st;
  CHECK_FALSE(write_metrics_json("build/no_such_dir_xyz/metrics.json", e, st, symbols));
}


// ========================================================= graceful shutdown

namespace {

volatile std::sig_atomic_t g_test_signal_flag = 0;
extern "C" void test_signal_handler(int) { g_test_signal_flag = 1; }

}  // namespace

TEST(signal_handler_sets_the_stop_flag) {
  // The production handler does exactly one thing -- set a sig_atomic_t -- and
  // a watcher thread turns that into Engine::request_stop(). Calling into the
  // engine from a signal context would not be safe. This verifies the
  // handler-to-flag half; the flag-to-engine half is covered below.
  //
  // Note: MSYS `kill -INT` cannot deliver a signal to a native Windows binary,
  // so this raises the signal in-process rather than shelling out.
  auto* previous = std::signal(SIGINT, test_signal_handler);
  CHECK(previous != SIG_ERR);
  g_test_signal_flag = 0;
  CHECK_EQ(static_cast<int>(g_test_signal_flag), 0);

  CHECK_EQ(std::raise(SIGINT), 0);
  CHECK_EQ(static_cast<int>(g_test_signal_flag), 1);

  std::signal(SIGINT, previous == SIG_ERR ? SIG_DFL : previous);
}

TEST(engine_stops_mid_run_when_asked_from_another_thread) {
  // The realistic shutdown path: the engine is already running when the stop
  // arrives, and must finish cleanly rather than being killed.
  EngineConfig cfg;
  cfg.threaded = true;
  cfg.risk.max_orders_per_second = 100'000'000;
  Engine e(cfg);

  SyntheticFeed::Params p;
  p.total_events = 50'000'000;  // far more than we intend to consume
  p.seed = 8080;
  SyntheticFeed feed(p);

  std::atomic<bool> done{false};
  std::thread stopper([&] {
    // Let it get going, then ask it to stop.
    while (!done.load(std::memory_order_acquire) && e.book().live_order_count() < 5000) {
      std::this_thread::yield();
    }
    e.request_stop();
  });

  const EngineStats st = e.run(feed);
  done.store(true, std::memory_order_release);
  stopper.join();

  CHECK(e.stop_requested());
  CHECK(st.ticks > 0);
  CHECK(st.ticks < p.total_events);   // stopped early, did not drain the feed
  CHECK_EQ(st.dropped_ticks, std::uint64_t(0));  // clean stop loses nothing

  // The engine must still be in a consistent, reportable state afterwards.
  const Price bb = e.book().best_bid();
  const Price ba = e.book().best_ask();
  if (bb != kNoPrice && ba != kNoPrice) CHECK(bb < ba);
  CHECK_EQ(e.venue().fill_count(), st.orders_sent);
}

TEST(engine_flatten_is_idempotent_and_safe_when_flat) {
  EngineConfig cfg;
  cfg.threaded = false;
  Engine e(cfg);
  EngineStats st;
  // Nothing traded yet: flattening must be a no-op, not a crash or a phantom
  // order priced at zero.
  CHECK_EQ(e.flatten(st), std::uint64_t(0));
  CHECK_EQ(st.flatten_orders, std::uint64_t(0));
  CHECK_EQ(e.venue().fill_count(), std::uint64_t(0));
}

TEST(engine_flatten_after_a_halt_still_exits_the_position) {
  // The kill switch blocks new risk, but must not trap us in an open position:
  // flattening deliberately bypasses the pre-trade gate.
  EngineConfig cfg;
  cfg.threaded = false;
  cfg.order_quantity = 10;
  cfg.risk.max_orders_per_second = 100'000'000;
  Engine e(cfg);

  SyntheticFeed::Params p;
  p.total_events = 100000;
  p.seed = 24680;
  SyntheticFeed feed(p);
  EngineStats st = e.run(feed);

  e.risk().engage_kill_switch(HaltReason::Manual);
  CHECK(e.risk().halted());
  CHECK(e.risk().should_flatten());

  const std::int64_t before = e.venue().position(0);
  e.flatten(st);
  CHECK_EQ(e.venue().position(0), std::int64_t(0));
  if (before != 0) CHECK(st.flatten_orders > 0);
}

// ====================================================================== main

int main() {
  std::printf("\nhft C++ engine -- unit tests\n\n");
  return testing::run_all();
}
