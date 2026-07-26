// Instrument registry tests.
//
// Every check here exists because the alternative is a rejected order at the
// venue or, worse, a book sized for the wrong instrument.

#include <string>

#include "hft/instrument.hpp"
#include "test_harness.hpp"

using namespace hft;

namespace {

Instrument basic(const char* name, Price lo = 5000, Price hi = 15000) {
  Instrument i;
  i.symbol = name;
  i.min_price = lo;
  i.max_price = hi;
  return i;
}

}  // namespace

// ================================================================ registry

TEST(instrument_registry_assigns_dense_ids_in_order) {
  InstrumentRegistry reg;
  std::string err;
  CHECK(reg.add(basic("AAA"), err));
  CHECK(reg.add(basic("BBB"), err));
  CHECK(reg.add(basic("CCC"), err));

  CHECK_EQ(reg.size(), std::size_t(3));
  CHECK_EQ(reg.find(SymbolId(0))->symbol, std::string("AAA"));
  CHECK_EQ(reg.find(SymbolId(2))->symbol, std::string("CCC"));
  CHECK_EQ(reg.find("BBB")->id, SymbolId(1));
  CHECK(reg.find(SymbolId(3)) == nullptr);
  CHECK(reg.find("DDD") == nullptr);
}

TEST(instrument_registry_rejects_a_duplicate_symbol) {
  // Two ids for one symbol would silently split a position in half.
  InstrumentRegistry reg;
  std::string err;
  CHECK(reg.add(basic("AAA"), err));
  CHECK_FALSE(reg.add(basic("AAA"), err));
  CHECK(err.find("duplicate") != std::string::npos);
  CHECK_EQ(reg.size(), std::size_t(1));
}

TEST(instrument_registry_rejects_a_nonsense_price_band) {
  InstrumentRegistry reg;
  std::string err;
  CHECK_FALSE(reg.add(basic("A", 15000, 5000), err));  // inverted
  CHECK_FALSE(reg.add(basic("B", 0, 5000), err));      // non-positive
  CHECK_FALSE(reg.add(basic("C", 5000, 5000), err));   // empty
}

TEST(instrument_registry_rejects_a_band_wider_than_the_book_can_index) {
  // The level bitmap is three tiers of 64 bits, so the band has a hard ceiling.
  // Failing here beats failing inside the book at run time.
  InstrumentRegistry reg;
  std::string err;
  CHECK_FALSE(reg.add(basic("WIDE", 1, 300'000), err));
  CHECK(err.find("supports at most") != std::string::npos);
  CHECK(reg.add(basic("OK", 1, 262'144), err));
}

TEST(instrument_registry_rejects_non_positive_tick_and_lot_sizes) {
  InstrumentRegistry reg;
  std::string err;
  Instrument bad_tick = basic("A");
  bad_tick.tick_size = 0;
  CHECK_FALSE(reg.add(bad_tick, err));

  Instrument bad_lot = basic("B");
  bad_lot.lot_size = -5;
  CHECK_FALSE(reg.add(bad_lot, err));

  Instrument bad_pos = basic("C");
  bad_pos.max_position = -1;
  CHECK_FALSE(reg.add(bad_pos, err));
}

TEST(instrument_registry_rejects_a_band_with_no_valid_price_on_its_tick_grid) {
  // A 25-tick grid inside the band 10001..10010 admits no price at all, which
  // would show up at run time as "every order is rejected".
  InstrumentRegistry reg;
  std::string err;
  Instrument i = basic("GRID", 10001, 10010);
  i.tick_size = 25;
  CHECK_FALSE(reg.add(i, err));
  CHECK(err.find("tick grid") != std::string::npos);
}

TEST(instrument_registry_reports_the_widest_band) {
  InstrumentRegistry reg;
  std::string err;
  reg.add(basic("A", 5000, 9000), err);
  reg.add(basic("B", 1000, 3000), err);
  reg.add(basic("C", 8000, 20000), err);
  CHECK_EQ(reg.min_price(), Price(1000));
  CHECK_EQ(reg.max_price(), Price(20000));
}

TEST(instrument_registry_clear_resets_names_too) {
  // Clearing the vector but not the name map would make a re-added symbol
  // resolve to a stale id.
  InstrumentRegistry reg;
  std::string err;
  reg.add(basic("AAA"), err);
  reg.clear();
  CHECK(reg.empty());
  CHECK(reg.find("AAA") == nullptr);
  CHECK(reg.add(basic("AAA"), err));
  CHECK_EQ(reg.find("AAA")->id, SymbolId(0));
}

// ============================================================== validation

TEST(instrument_validates_prices_against_band_and_tick_grid) {
  Instrument i = basic("A", 10000, 20000);
  i.tick_size = 25;

  CHECK(i.price_is_valid(10000));
  CHECK(i.price_is_valid(10025));
  CHECK_FALSE(i.price_is_valid(10001));  // off the grid
  CHECK_FALSE(i.price_is_valid(9975));   // below the band
  CHECK_FALSE(i.price_is_valid(20025));  // above the band

  Instrument any = basic("B", 10000, 20000);  // tick_size 1
  CHECK(any.price_is_valid(10001));
}

TEST(instrument_validates_quantities_against_lot_size) {
  Instrument i = basic("A");
  i.lot_size = 100;
  CHECK(i.quantity_is_valid(100));
  CHECK(i.quantity_is_valid(500));
  CHECK_FALSE(i.quantity_is_valid(150));
  CHECK_FALSE(i.quantity_is_valid(0));
  CHECK_FALSE(i.quantity_is_valid(-100));
}

TEST(instrument_rounds_prices_toward_the_passive_side) {
  // Rounding must never push an order further into the market than intended.
  Instrument i = basic("A", 10000, 20000);
  i.tick_size = 25;

  CHECK_EQ(i.round_price(10013, Side::Buy), Price(10000));   // down
  CHECK_EQ(i.round_price(10013, Side::Sell), Price(10025));  // up
  // An already-valid price is untouched on both sides.
  CHECK_EQ(i.round_price(10025, Side::Buy), Price(10025));
  CHECK_EQ(i.round_price(10025, Side::Sell), Price(10025));

  Instrument any = basic("B");
  CHECK_EQ(any.round_price(10013, Side::Buy), Price(10013));
}

// =================================================================== specs

TEST(instrument_spec_parses_the_full_form) {
  Instrument i;
  std::string err;
  CHECK(parse_instrument("ESZ5:400000:500000:25:1:250", i, err));
  CHECK_EQ(i.symbol, std::string("ESZ5"));
  CHECK_EQ(i.min_price, Price(400000));
  CHECK_EQ(i.max_price, Price(500000));
  CHECK_EQ(i.tick_size, Price(25));
  CHECK_EQ(i.lot_size, Quantity(1));
  CHECK_EQ(i.max_position, std::int64_t(250));
}

TEST(instrument_spec_defaults_the_optional_fields) {
  Instrument i;
  std::string err;
  CHECK(parse_instrument("AAPL:15000:30000", i, err));
  CHECK_EQ(i.tick_size, Price(1));
  CHECK_EQ(i.lot_size, Quantity(1));
  CHECK_EQ(i.max_position, std::int64_t(0));  // 0 means "use the global limit"
}

TEST(instrument_spec_rejects_malformed_input) {
  Instrument i;
  std::string err;
  CHECK_FALSE(parse_instrument("AAPL", i, err));            // too few fields
  CHECK_FALSE(parse_instrument("AAPL:1:2:3:4:5:6", i, err));  // too many
  CHECK_FALSE(parse_instrument(":1:2", i, err));             // no symbol
  // Garbage in a numeric field must be an error, never a silent zero.
  CHECK_FALSE(parse_instrument("AAPL:100abc:200", i, err));
  CHECK(err.find("min_price") != std::string::npos);
  CHECK_FALSE(parse_instrument("AAPL:100:2oo", i, err));
  CHECK_FALSE(parse_instrument("AAPL::200", i, err));
}

TEST(instrument_summary_lists_every_instrument) {
  InstrumentRegistry reg;
  std::string err;
  Instrument es = basic("ESZ5", 400000, 450000);
  es.tick_size = 25;
  es.max_position = 250;
  reg.add(es, err);
  reg.add(basic("AAPL", 15000, 30000), err);

  const std::string text = reg.summary();
  CHECK(text.find("ESZ5") != std::string::npos);
  CHECK(text.find("AAPL") != std::string::npos);
  CHECK(text.find("max_position 250") != std::string::npos);
}
