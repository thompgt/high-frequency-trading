// Execution venue interface + paper simulator.
//
// Port of hft/execution/{base,paper}.py. The PnL accounting is deliberately
// identical to paper.py -- average-cost basis, realized PnL booked on the
// closing portion of a position, fees deducted from realized PnL -- so a run
// of the C++ engine and a run of the Python pipeline over the same fills
// agree to the cent.
//
// Two things the Python venue did not model are added here because the C++
// engine has a real book to work against:
//   * PaperVenue can fill against the live OrderBook (crossing the actual
//     spread and consuming real depth) instead of pretending every order
//     fills at the reference price.
//   * An equity curve is recorded per fill, which is what the demo notebook
//     charts.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "hft/order_book.hpp"
#include "hft/types.hpp"

namespace hft {

class ExecutionVenue {
 public:
  virtual ~ExecutionVenue() = default;
  // Submits an order and returns the resulting fill. `reference_price` is the
  // strategy's view of fair value at signal time.
  virtual Fill submit(const Order& order, Price reference_price) = 0;
  virtual const char* name() const = 0;
};

class PaperVenue : public ExecutionVenue {
 public:
  struct Config {
    double slippage_bps = 1.0;
    double fee_bps = 0.5;
    // When set, marketable orders are matched against this book and the fill
    // price is the volume-weighted price actually obtained. When null, the
    // venue falls back to reference_price +/- slippage, exactly like paper.py.
    OrderBook* book = nullptr;
  };

  struct PnlPoint {
    Nanos ts_ns = 0;
    std::uint64_t fill_index = 0;
    double realized_pnl = 0.0;
    double fees_paid = 0.0;
    Price fill_price = 0;
    Side side = Side::Buy;
    Quantity quantity = 0;
    std::int64_t position = 0;
  };

  explicit PaperVenue(Config config);

  Fill submit(const Order& order, Price reference_price) override;
  const char* name() const override { return "PaperVenue"; }

  double realized_pnl() const { return realized_pnl_; }
  double fees_paid() const { return fees_paid_; }
  std::int64_t position(SymbolId symbol) const;
  const std::unordered_map<SymbolId, std::int64_t>& positions() const { return positions_; }
  std::uint64_t fill_count() const { return fill_count_; }

  // Marks open positions at `price` and returns realized + unrealized PnL.
  double equity(SymbolId symbol, Price mark_price) const;

  const std::vector<PnlPoint>& equity_curve() const { return curve_; }
  void set_record_curve(bool on) { record_curve_ = on; }

  bool write_fills_csv(const std::string& path) const;

 private:
  Price fill_price_for(const Order& order, Price reference_price, Quantity& filled_qty);
  void apply_pnl(SymbolId symbol, Side side, Quantity qty, double fill_px, double fee);

  Config cfg_;
  std::unordered_map<SymbolId, std::int64_t> positions_;
  std::unordered_map<SymbolId, double> cost_basis_;  // sum of fill_px * signed_qty
  double realized_pnl_ = 0.0;
  double fees_paid_ = 0.0;
  std::uint64_t fill_count_ = 0;
  bool record_curve_ = true;
  std::vector<PnlPoint> curve_;
  OrderId next_venue_order_id_ = 1'000'000'000ULL;
};

}  // namespace hft
