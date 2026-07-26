// Multi-instrument engine tests.
//
// The failure this guards against is subtle and expensive: one instrument's
// state leaking into another's. A shared book, a shared fair value, or a
// shared limit all produce plausible-looking numbers that are simply wrong.

#include <fstream>
#include <string>
#include <vector>

#include "hft/engine.hpp"
#include "hft/feed.hpp"
#include "hft/instrument.hpp"
#include "hft/risk.hpp"
#include "test_harness.hpp"

using namespace hft;

namespace {

// Three instruments with genuinely different contracts: different price bands,
// a tick grid, and a tightened position limit.
EngineConfig multi_config() {
  EngineConfig cfg;
  cfg.threaded = false;
  cfg.fast_window = 3;
  cfg.slow_window = 8;
  cfg.order_quantity = 10;
  cfg.record_curve = false;
  cfg.risk.max_orders_per_second = 1000000000u;
  cfg.risk.max_position_per_symbol = 100000;

  std::string err;
  Instrument a;
  a.symbol = "CHEAP";
  a.min_price = 100;
  a.max_price = 900;
  CHECK(cfg.instruments.add(a, err));

  Instrument b;
  b.symbol = "MID";
  b.min_price = 9000;
  b.max_price = 11000;
  CHECK(cfg.instruments.add(b, err));

  Instrument c;
  c.symbol = "PRICEY";
  c.min_price = 400000;
  c.max_price = 410000;
  c.tick_size = 25;
  c.max_position = 40;  // deliberately tighter than the global limit
  CHECK(cfg.instruments.add(c, err));
  return cfg;
}

SyntheticFeed::Params sym_params(SymbolId id, Price lo, Price hi, std::uint64_t seed) {
  SyntheticFeed::Params p;
  p.symbol = id;
  p.min_price = lo;
  p.max_price = hi;
  p.start_price = (lo + hi) / 2;
  p.tick_range = 20;
  p.seed = seed;
  p.total_events = 40000;
  return p;
}

std::vector<SyntheticFeed::Params> three_feeds() {
  return {sym_params(0, 100, 900, 0xAAA1), sym_params(1, 9000, 11000, 0xBBB2),
          sym_params(2, 400000, 410000, 0xCCC3)};
}

Tick book_tick(SymbolId symbol, Price price, Quantity qty, OrderId id, std::uint64_t seq) {
  Tick t{};
  t.symbol = symbol;
  t.type = TickType::AddOrder;
  t.side = Side::Buy;
  t.price = price;
  t.quantity = qty;
  t.order_id = id;
  t.sequence = seq;
  t.ingest_ts_ns = static_cast<Nanos>(seq);
  return t;
}

}  // namespace

// ============================================================ separate books

TEST(engine_gives_each_instrument_its_own_book) {
  Engine engine(multi_config());
  CHECK_EQ(engine.book_count(), std::size_t(3));

  // Each book covers its own band. A $4000 instrument and a $5 one could never
  // usefully share one.
  CHECK_EQ(engine.book(0).min_price(), Price(100));
  CHECK_EQ(engine.book(0).max_price(), Price(900));
  CHECK_EQ(engine.book(2).min_price(), Price(400000));
  CHECK_EQ(engine.book(2).max_price(), Price(410000));
  CHECK_THROWS(engine.book(3));
}

TEST(engine_still_works_with_no_instruments_configured) {
  // The single-symbol case must stay a one-liner: no instruments configured
  // means one synthesised from the price band.
  EngineConfig cfg;
  cfg.threaded = false;
  cfg.min_price = 5000;
  cfg.max_price = 15000;
  Engine engine(cfg);

  CHECK_EQ(engine.book_count(), std::size_t(1));
  CHECK_EQ(engine.instruments().size(), std::size_t(1));
  CHECK_EQ(engine.book().min_price(), Price(5000));
  CHECK_EQ(engine.book().max_price(), Price(15000));
}

TEST(engine_keeps_instrument_books_completely_separate) {
  Engine engine(multi_config());
  MultiSymbolFeed feed(three_feeds());
  const EngineStats stats = engine.run(feed);

  CHECK(stats.ticks > 0);
  CHECK_EQ(stats.unknown_symbol_ticks, std::uint64_t(0));

  // Every book's prices sit inside its own band, so nothing crossed over.
  for (SymbolId s = 0; s < 3; ++s) {
    const OrderBook& book = engine.book(s);
    const Price bid = book.best_bid();
    const Price ask = book.best_ask();
    if (bid != kNoPrice) {
      CHECK(bid >= book.min_price());
      CHECK(bid <= book.max_price());
    }
    if (ask != kNoPrice) {
      CHECK(ask >= book.min_price());
      CHECK(ask <= book.max_price());
    }
  }
  // One channel, so sequence numbers are contiguous across all three symbols.
  CHECK(engine.feed_monitor().healthy());
  CHECK_EQ(engine.feed_monitor().stats().gaps, std::uint64_t(0));
}

// ============================================================== validation

TEST(engine_drops_messages_for_a_symbol_it_does_not_trade) {
  Engine engine(multi_config());
  EngineStats stats;
  engine.process(book_tick(47, 10000, 10, 1, 1), stats);

  CHECK_EQ(stats.unknown_symbol_ticks, std::uint64_t(1));
  CHECK_EQ(stats.adds, std::uint64_t(0));
  // Nothing was invented for it.
  CHECK_EQ(engine.book_count(), std::size_t(3));
}

TEST(engine_rejects_book_messages_that_violate_the_instrument_contract) {
  Engine engine(multi_config());
  EngineStats stats;

  // PRICEY trades on a 25-tick grid, and 400013 is not on it.
  engine.process(book_tick(2, 400013, 5, 1, 1), stats);
  CHECK_EQ(stats.contract_violations, std::uint64_t(1));
  CHECK_EQ(engine.book(2).live_order_count(), std::size_t(0));

  // The same price on the grid is accepted.
  engine.process(book_tick(2, 400025, 5, 2, 2), stats);
  CHECK_EQ(engine.book(2).live_order_count(), std::size_t(1));
  CHECK_EQ(stats.contract_violations, std::uint64_t(1));
}

TEST(engine_rejects_a_quantity_that_is_not_a_whole_lot) {
  EngineConfig cfg = multi_config();
  std::string err;
  Instrument lots;
  lots.symbol = "LOTS";
  lots.min_price = 9000;
  lots.max_price = 11000;
  lots.lot_size = 100;
  CHECK(cfg.instruments.add(lots, err));
  Engine engine(cfg);
  EngineStats stats;

  engine.process(book_tick(3, 10000, 150, 1, 1), stats);  // 1.5 lots
  CHECK_EQ(stats.contract_violations, std::uint64_t(1));
  CHECK_EQ(engine.book(3).live_order_count(), std::size_t(0));

  engine.process(book_tick(3, 10000, 200, 2, 2), stats);  // 2 lots
  CHECK_EQ(engine.book(3).live_order_count(), std::size_t(1));
}

// ============================================================ risk limits

TEST(engine_applies_the_tighter_per_instrument_position_limit) {
  Engine engine(multi_config());
  // The global limit is 100000; PRICEY declares 40, which must win.
  CHECK_EQ(engine.risk().symbol_limit(2), std::int64_t(40));
  // Instruments without their own limit fall back to the global one.
  CHECK_EQ(engine.risk().symbol_limit(0), std::int64_t(100000));
  CHECK_EQ(engine.risk().symbol_limit(1), std::int64_t(100000));
}

TEST(risk_per_symbol_limit_can_only_tighten_the_global_one) {
  // A config mistake must never be able to widen a risk limit.
  RiskLimits limits;
  limits.max_position_per_symbol = 500;
  RiskManager risk(limits);

  risk.set_symbol_limit(0, 100);
  CHECK_EQ(risk.symbol_limit(0), std::int64_t(100));

  risk.set_symbol_limit(1, 10000);  // tries to loosen
  CHECK_EQ(risk.symbol_limit(1), std::int64_t(500));

  risk.set_symbol_limit(0, 0);  // 0 means "use the global limit"
  CHECK_EQ(risk.symbol_limit(0), std::int64_t(500));

  // An unconfigured symbol uses the global limit too.
  CHECK_EQ(risk.symbol_limit(99), std::int64_t(500));
}

TEST(risk_enforces_the_per_symbol_limit_on_the_check_path) {
  RiskLimits limits;
  limits.max_position_per_symbol = 1000;
  limits.max_gross_position = 1000000;
  RiskManager risk(limits);
  risk.set_symbol_limit(1, 50);

  Order o{};
  o.type = OrderType::Market;
  o.side = Side::Buy;
  o.price = 10000;
  o.quantity = 100;

  o.symbol = 0;  // no specific limit: the global 1000 applies
  CHECK(risk.check(o, 10000, 0).accepted);

  o.symbol = 1;  // tightened to 50, so 100 is too many
  const RiskDecision d = risk.check(o, 10000, 0);
  CHECK_FALSE(d.accepted);
  CHECK_EQ(int(d.reason), int(RejectReason::PositionLimitExceeded));
}

// =============================================================== the feed

TEST(multi_symbol_feed_numbers_the_channel_not_the_instrument) {
  std::vector<SyntheticFeed::Params> params = {sym_params(0, 100, 900, 1),
                                               sym_params(1, 9000, 11000, 2)};
  for (auto& p : params) p.total_events = 100;
  MultiSymbolFeed feed(params);

  Tick t;
  std::uint64_t expected = 1;
  std::size_t from_zero = 0;
  std::size_t from_one = 0;
  while (feed.next(t)) {
    CHECK_EQ(t.sequence, expected);  // contiguous across both instruments
    ++expected;
    (t.symbol == 0 ? from_zero : from_one) += 1;
  }
  CHECK_EQ(from_zero, std::size_t(100));
  CHECK_EQ(from_one, std::size_t(100));
  CHECK_EQ(expected, std::uint64_t(201));
}

TEST(multi_symbol_feed_drains_instruments_that_run_out_early) {
  // Uneven event counts must not stall the channel or repeat a symbol's last
  // message.
  std::vector<SyntheticFeed::Params> params = {sym_params(0, 100, 900, 1),
                                               sym_params(1, 9000, 11000, 2)};
  params[0].total_events = 5;
  params[1].total_events = 50;
  MultiSymbolFeed feed(params);

  Tick t;
  std::size_t total = 0;
  std::size_t from_zero = 0;
  while (feed.next(t)) {
    ++total;
    if (t.symbol == 0) ++from_zero;
  }
  CHECK_EQ(total, std::size_t(55));
  CHECK_EQ(from_zero, std::size_t(5));
}

TEST(multi_symbol_feed_is_deterministic) {
  MultiSymbolFeed a(three_feeds());
  MultiSymbolFeed b(three_feeds());
  Tick ta, tb;
  for (int i = 0; i < 5000; ++i) {
    CHECK(a.next(ta));
    CHECK(b.next(tb));
    CHECK_EQ(ta.symbol, tb.symbol);
    CHECK_EQ(ta.price, tb.price);
    CHECK_EQ(ta.quantity, tb.quantity);
    CHECK_EQ(ta.sequence, tb.sequence);
  }
}

// ============================================================== shutdown

TEST(engine_flattens_each_instrument_against_its_own_book) {
  // Marking one instrument's exit at another's price would book fictional PnL.
  // A $4 instrument closed at $4000 would look spectacularly profitable.
  Engine engine(multi_config());
  MultiSymbolFeed feed(three_feeds());
  EngineStats stats = engine.run(feed);

  engine.flatten(stats);
  for (const auto& kv : engine.venue().positions()) {
    CHECK_EQ(kv.second, std::int64_t(0));
  }
  CHECK_EQ(engine.oms().gross_working_exposure(), std::int64_t(0));
  CHECK_EQ(engine.oms().stats().breaks(), std::uint64_t(0));
}

TEST(engine_book_snapshot_names_every_instrument) {
  Engine engine(multi_config());
  MultiSymbolFeed feed(three_feeds());
  EngineStats stats = engine.run(feed);
  (void)stats;

  const std::string path = "build/multi_snapshot.csv";
  CHECK(engine.write_book_snapshot_csv(path, 3));

  std::ifstream in(path);
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  // Without the symbol column, three books' depth would be indistinguishable.
  CHECK(text.find("symbol,side,price") != std::string::npos);
  CHECK(text.find("CHEAP") != std::string::npos);
  CHECK(text.find("MID") != std::string::npos);
  CHECK(text.find("PRICEY") != std::string::npos);
}
