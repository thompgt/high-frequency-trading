// Startup reconciliation: our recovered view against the venue's.
//
// The cases that matter here are the disagreements, not the agreements. A
// reconciler that only ever gets tested on matching inputs is a reconciler
// that has never done its job.

#include <cstdio>
#include <fstream>
#include <string>

#include "hft/reconcile.hpp"
#include "test_harness.hpp"

using namespace hft;

namespace {

OrderRecord working_order(ClOrdId id, SymbolId symbol, Side side, Price price, Quantity leaves,
                          OrderState state = OrderState::New) {
  OrderRecord r;
  r.cl_ord_id = id;
  r.venue_order_id = 900 + id;
  r.symbol = symbol;
  r.side = side;
  r.price = price;
  r.quantity = leaves;
  r.leaves = leaves;
  r.state = state;
  return r;
}

VenueOrder venue_order(ClOrdId id, SymbolId symbol, Side side, Price price, Quantity leaves) {
  VenueOrder v;
  v.cl_ord_id = id;
  v.venue_order_id = 900 + id;
  v.symbol = symbol;
  v.side = side;
  v.price = price;
  v.quantity = leaves;
  v.leaves = leaves;
  return v;
}

std::string temp_path(const char* name) {
  return std::string("venue_state_test_") + name + ".txt";
}

void write_file(const std::string& path, const std::string& body) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
}

}  // namespace

// ============================================================ availability

TEST(reconcile_treats_an_unreachable_venue_as_not_reconciled) {
  RecoveredState ours;
  VenueSnapshot theirs;  // available stays false
  const ReconciliationReport report = reconcile(ours, theirs);
  CHECK_FALSE(report.venue_available);
  CHECK_FALSE(report.clean());
  // Nothing was compared, so nothing may be claimed about the comparison.
  CHECK_EQ(report.discrepancies.size(), std::size_t(0));
  CHECK_EQ(report.orders_ours, std::size_t(0));
}

TEST(reconcile_of_two_empty_but_reachable_sides_is_clean) {
  RecoveredState ours;
  VenueSnapshot theirs;
  theirs.available = true;
  const ReconciliationReport report = reconcile(ours, theirs);
  CHECK(report.clean());
}

// ================================================================== orders

TEST(reconcile_matches_orders_both_sides_agree_on) {
  RecoveredState ours;
  ours.open_orders.push_back(working_order(7, 0, Side::Buy, 10000, 50));
  VenueSnapshot theirs;
  theirs.available = true;
  theirs.open_orders.push_back(venue_order(7, 0, Side::Buy, 10000, 50));

  const ReconciliationReport report = reconcile(ours, theirs);
  CHECK(report.clean());
  CHECK_EQ(report.orders_agreed, std::size_t(1));
  // An agreed order is still a live order: it comes back so the engine can
  // adopt it rather than leave it untracked.
  CHECK_EQ(report.agreed_open.size(), std::size_t(1));
  CHECK_EQ(report.agreed_open[0].cl_ord_id, ClOrdId(7));
}

TEST(reconcile_flags_an_order_live_at_the_venue_that_we_do_not_track) {
  RecoveredState ours;
  VenueSnapshot theirs;
  theirs.available = true;
  theirs.open_orders.push_back(venue_order(42, 0, Side::Sell, 10100, 25));

  const ReconciliationReport report = reconcile(ours, theirs);
  CHECK_FALSE(report.clean());
  CHECK_EQ(report.count(BreakKind::OrphanAtVenue), std::size_t(1));
  CHECK_EQ(report.discrepancies[0].cl_ord_id, ClOrdId(42));
  CHECK_EQ(report.discrepancies[0].theirs, std::int64_t(25));
}

TEST(reconcile_flags_an_order_we_think_is_working_that_the_venue_has_lost) {
  RecoveredState ours;
  ours.open_orders.push_back(working_order(3, 0, Side::Buy, 9900, 10));
  VenueSnapshot theirs;
  theirs.available = true;

  const ReconciliationReport report = reconcile(ours, theirs);
  CHECK_EQ(report.count(BreakKind::MissingAtVenue), std::size_t(1));
  CHECK_EQ(report.orders_ours, std::size_t(1));
  CHECK_EQ(report.orders_theirs, std::size_t(0));
}

TEST(reconcile_flags_a_leaves_disagreement) {
  RecoveredState ours;
  ours.open_orders.push_back(working_order(5, 0, Side::Buy, 10000, 100));
  VenueSnapshot theirs;
  theirs.available = true;
  VenueOrder vo = venue_order(5, 0, Side::Buy, 10000, 100);
  vo.filled = 40;  // a fill report we never saw
  vo.leaves = 60;
  theirs.open_orders.push_back(vo);

  const ReconciliationReport report = reconcile(ours, theirs);
  CHECK_EQ(report.count(BreakKind::QuantityMismatch), std::size_t(1));
  CHECK_EQ(report.discrepancies[0].ours, std::int64_t(100));
  CHECK_EQ(report.discrepancies[0].theirs, std::int64_t(60));
  // A mismatched order is not agreed, so it must not be offered for adoption.
  CHECK_EQ(report.agreed_open.size(), std::size_t(0));
}

TEST(reconcile_flags_a_price_or_side_disagreement_as_terms_not_quantity) {
  RecoveredState ours;
  ours.open_orders.push_back(working_order(9, 0, Side::Buy, 10000, 30));
  VenueSnapshot theirs;
  theirs.available = true;
  theirs.open_orders.push_back(venue_order(9, 0, Side::Sell, 10000, 30));

  const ReconciliationReport report = reconcile(ours, theirs);
  CHECK_EQ(report.count(BreakKind::TermsMismatch), std::size_t(1));
  CHECK_EQ(report.count(BreakKind::QuantityMismatch), std::size_t(0));
}

TEST(reconcile_ignores_orders_we_already_consider_finished) {
  RecoveredState ours;
  ours.open_orders.push_back(working_order(1, 0, Side::Buy, 10000, 0, OrderState::Filled));
  VenueSnapshot theirs;
  theirs.available = true;

  const ReconciliationReport report = reconcile(ours, theirs);
  CHECK(report.clean());
  CHECK_EQ(report.orders_ours, std::size_t(0));
}

TEST(reconcile_flags_a_venue_order_with_no_client_id) {
  RecoveredState ours;
  VenueSnapshot theirs;
  theirs.available = true;
  theirs.open_orders.push_back(venue_order(0, 0, Side::Buy, 10000, 5));

  const ReconciliationReport report = reconcile(ours, theirs);
  CHECK_EQ(report.count(BreakKind::OrphanAtVenue), std::size_t(1));
}

TEST(reconcile_flags_two_venue_orders_sharing_one_client_id) {
  RecoveredState ours;
  ours.open_orders.push_back(working_order(11, 0, Side::Buy, 10000, 20));
  VenueSnapshot theirs;
  theirs.available = true;
  theirs.open_orders.push_back(venue_order(11, 0, Side::Buy, 10000, 20));
  theirs.open_orders.push_back(venue_order(11, 0, Side::Buy, 10000, 20));

  const ReconciliationReport report = reconcile(ours, theirs);
  // The first matches; the duplicate must not be silently absorbed by it.
  CHECK_EQ(report.orders_agreed, std::size_t(1));
  CHECK_EQ(report.count(BreakKind::OrphanAtVenue), std::size_t(1));
}

// =============================================================== positions

TEST(reconcile_compares_positions_across_the_union_of_both_sides) {
  RecoveredState ours;
  ours.positions = {100, 0, -50};
  VenueSnapshot theirs;
  theirs.available = true;
  theirs.positions.push_back({0, 100});
  theirs.positions.push_back({2, -50});
  CHECK(reconcile(ours, theirs).clean());

  // A symbol the venue holds and we have no record of at all is exactly the
  // case a "walk our positions" loop would miss.
  theirs.positions.push_back({5, 7});
  const ReconciliationReport report = reconcile(ours, theirs);
  CHECK_EQ(report.count(BreakKind::PositionMismatch), std::size_t(1));
  CHECK_EQ(report.discrepancies[0].symbol, SymbolId(5));
  CHECK_EQ(report.discrepancies[0].ours, std::int64_t(0));
  CHECK_EQ(report.discrepancies[0].theirs, std::int64_t(7));
}

TEST(reconcile_sums_repeated_position_lines_for_one_symbol) {
  RecoveredState ours;
  ours.positions = {30};
  VenueSnapshot theirs;
  theirs.available = true;
  theirs.positions.push_back({0, 10});
  theirs.positions.push_back({0, 20});
  CHECK(reconcile(ours, theirs).clean());
}

// ============================================================= file source

TEST(file_venue_state_source_reads_orders_and_positions) {
  const std::string path = temp_path("basic");
  write_file(path,
             "# what the venue reported at 06:02\n"
             "\n"
             "ORDER,17,555,0,BUY,10000,100,40,60\n"
             "POSITION,0,-250\n");
  FileVenueStateSource source(path, nullptr);
  VenueSnapshot snap;
  std::string error;
  CHECK(source.fetch(snap, error));
  CHECK(snap.available);
  CHECK_EQ(snap.open_orders.size(), std::size_t(1));
  CHECK_EQ(snap.open_orders[0].cl_ord_id, ClOrdId(17));
  CHECK_EQ(snap.open_orders[0].leaves, Quantity(60));
  CHECK_EQ(snap.positions.size(), std::size_t(1));
  CHECK_EQ(snap.positions[0].position, std::int64_t(-250));
  std::remove(path.c_str());
}

TEST(file_venue_state_source_resolves_symbol_names_through_the_registry) {
  InstrumentRegistry registry;
  std::string error;
  Instrument a;
  a.symbol = "AAPL";
  CHECK(registry.add(a, error));
  Instrument b;
  b.symbol = "MSFT";
  CHECK(registry.add(b, error));

  const std::string path = temp_path("names");
  write_file(path, "ORDER,1,2,MSFT,SELL,10000,10,0,10\nPOSITION,AAPL,5\n");
  FileVenueStateSource source(path, &registry);
  VenueSnapshot snap;
  CHECK(source.fetch(snap, error));
  CHECK_EQ(snap.open_orders[0].symbol, SymbolId(1));
  CHECK_EQ(snap.positions[0].symbol, SymbolId(0));
  std::remove(path.c_str());
}

TEST(file_venue_state_source_rejects_an_unknown_symbol_rather_than_skipping_it) {
  InstrumentRegistry registry;
  std::string error;
  Instrument a;
  a.symbol = "AAPL";
  CHECK(registry.add(a, error));

  const std::string path = temp_path("badsym");
  write_file(path, "ORDER,1,2,APPL,SELL,10000,10,0,10\n");
  FileVenueStateSource source(path, &registry);
  VenueSnapshot snap;
  CHECK_FALSE(source.fetch(snap, error));
  // Unavailable, not empty: a half-read snapshot must never look like "the
  // venue has nothing open".
  CHECK_FALSE(snap.available);
  CHECK(error.find("APPL") != std::string::npos);
  std::remove(path.c_str());
}

TEST(file_venue_state_source_rejects_impossible_quantities) {
  const std::string path = temp_path("badqty");
  write_file(path, "ORDER,1,2,0,BUY,10000,10,8,8\n");  // 8 + 8 > 10
  FileVenueStateSource source(path, nullptr);
  VenueSnapshot snap;
  std::string error;
  CHECK_FALSE(source.fetch(snap, error));
  CHECK_FALSE(snap.available);
  std::remove(path.c_str());
}

TEST(file_venue_state_source_reports_a_missing_file_as_unavailable) {
  FileVenueStateSource source("venue_state_test_does_not_exist.txt", nullptr);
  VenueSnapshot snap;
  std::string error;
  CHECK_FALSE(source.fetch(snap, error));
  CHECK_FALSE(snap.available);
  CHECK_FALSE(error.empty());
}

// ================================================================= adoption

TEST(oms_adopts_an_order_the_venue_confirmed_is_still_resting) {
  OrderManager oms;
  OrderRecord snapshot = working_order(31, 0, Side::Buy, 10000, 60);
  snapshot.quantity = 100;
  snapshot.filled = 40;
  snapshot.state = OrderState::PartiallyFilled;

  CHECK_EQ(oms.adopt(snapshot, 1000), ClOrdId(31));
  const OrderRecord* rec = oms.find(31);
  CHECK(rec != nullptr);
  CHECK_EQ(int(rec->state), int(OrderState::PartiallyFilled));
  // Exposure must be the 60 still resting, not the original 100.
  CHECK_EQ(oms.working_exposure(0), std::int64_t(60));
  CHECK_EQ(oms.stats().adopted, std::uint64_t(1));
  CHECK_EQ(oms.stats().created, std::uint64_t(0));
  // The id sequence must move past an adopted id, or the next order we send
  // reuses it.
  CHECK(oms.next_id() > ClOrdId(31));
}

TEST(oms_can_fill_and_cancel_an_adopted_order_like_any_other) {
  OrderManager oms;
  CHECK_EQ(oms.adopt(working_order(8, 0, Side::Sell, 10100, 20), 1000), ClOrdId(8));

  ExecutionReport fill;
  fill.cl_ord_id = 8;
  fill.symbol = 0;
  fill.side = Side::Sell;
  fill.type = ExecType::Fill;
  fill.price = 10100;
  fill.quantity = 20;
  CHECK(oms.apply(fill));
  CHECK_EQ(int(oms.find(8)->state), int(OrderState::Filled));
  CHECK_EQ(oms.working_exposure(0), std::int64_t(0));
  CHECK_EQ(oms.stats().breaks(), std::uint64_t(0));
}

TEST(oms_does_not_expire_an_adopted_order) {
  OrderManager oms;
  CHECK_EQ(oms.adopt(working_order(12, 0, Side::Buy, 10000, 5), 1000), ClOrdId(12));
  // Far beyond the ack timeout. An adopted order has already been acked by the
  // venue -- expiring it would claim it never arrived.
  CHECK_EQ(oms.sweep_timeouts(1000 + 60'000'000'000LL), std::size_t(0));
  CHECK_EQ(int(oms.find(12)->state), int(OrderState::New));
}

TEST(oms_refuses_to_adopt_a_finished_order) {
  OrderManager oms;
  OrderRecord done = working_order(4, 0, Side::Buy, 10000, 0, OrderState::Filled);
  CHECK_EQ(oms.adopt(done, 1000), ClOrdId(0));
  CHECK_EQ(oms.open_count(), std::size_t(0));
  CHECK_EQ(oms.working_exposure(0), std::int64_t(0));
}

TEST(oms_refuses_to_adopt_an_id_it_is_already_tracking) {
  OrderManager oms;
  CHECK_EQ(oms.adopt(working_order(6, 0, Side::Buy, 10000, 5), 1000), ClOrdId(6));
  CHECK_EQ(oms.adopt(working_order(6, 0, Side::Buy, 10000, 5), 1000), ClOrdId(0));
  CHECK_EQ(oms.working_exposure(0), std::int64_t(5));
}
