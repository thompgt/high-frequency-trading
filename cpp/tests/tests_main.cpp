// Unit tests for the HFT engine. Run with `make test`.
//
// The order book gets the most coverage by a wide margin, including a
// randomised differential test against an independently-maintained shadow
// model -- matching logic is the one place where a subtle bug silently
// corrupts everything downstream.

#include <cstdio>
#include <map>
#include <numeric>
#include <thread>
#include <vector>

#include "hft/engine.hpp"
#include "hft/execution.hpp"
#include "hft/feed.hpp"
#include "hft/latency.hpp"
#include "hft/order_book.hpp"
#include "hft/ring_buffer.hpp"
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
  PaperVenue v(PaperVenue::Config{0.0, 0.0, &b});
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
  CHECK_EQ(st.orders_sent, st.signals);
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

// ====================================================================== main

int main() {
  std::printf("\nhft C++ engine -- unit tests\n\n");
  return testing::run_all();
}
