#include "hft/execution.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "hft/latency.hpp"

namespace hft {

PaperVenue::PaperVenue(Config config) : cfg_(config) { curve_.reserve(4096); }

std::int64_t PaperVenue::position(SymbolId symbol) const {
  auto it = positions_.find(symbol);
  return it == positions_.end() ? 0 : it->second;
}

// Determines the price this order actually gets, and how much of it fills.
Price PaperVenue::fill_price_for(const Order& order, Price reference_price, Quantity& filled_qty) {
  OrderBook* book = cfg_.books != nullptr ? cfg_.books->book_for(order.symbol) : nullptr;
  if (book != nullptr) {
    // Cross the real book -- this instrument's book. The order sweeps resting
    // liquidity, so the average price depends on how deep it has to reach,
    // which is what actual slippage is rather than a flat bps haircut.
    std::vector<Trade> trades;
    trades.reserve(8);
    const OrderId oid = next_venue_order_id_++;
    const Quantity got = book->execute_market(oid, order.side, order.quantity, &trades);
    if (got > 0) {
      std::int64_t notional = 0;
      for (const auto& t : trades) notional += t.price * t.quantity;
      filled_qty = got;
      return static_cast<Price>(notional / got);  // volume-weighted average
    }
    // Book was empty on that side: fall through to the reference-price model
    // rather than silently reporting a zero fill.
  }

  const double slip = static_cast<double>(reference_price) * (cfg_.slippage_bps / 10'000.0);
  const double px = (order.side == Side::Buy) ? static_cast<double>(reference_price) + slip
                                              : static_cast<double>(reference_price) - slip;
  filled_qty = order.quantity;
  return static_cast<Price>(px + (px >= 0 ? 0.5 : -0.5));
}

// Average-cost PnL accounting, mirroring hft/execution/paper.py exactly.
void PaperVenue::apply_pnl(SymbolId symbol, Side side, Quantity qty, double fill_px, double fee) {
  const std::int64_t signed_qty = (side == Side::Buy) ? qty : -qty;
  const std::int64_t prev_qty = position(symbol);
  const double prev_cost = cost_basis_.count(symbol) ? cost_basis_[symbol] : 0.0;

  std::int64_t new_qty;
  if (prev_qty == 0 || ((prev_qty > 0) == (signed_qty > 0))) {
    // Opening or adding to a position: no PnL is realized, cost basis grows.
    new_qty = prev_qty + signed_qty;
    cost_basis_[symbol] = prev_cost + fill_px * static_cast<double>(signed_qty);
  } else {
    // Reducing or flipping: book PnL on the portion actually closed.
    const std::int64_t closing_qty = std::min(std::abs(signed_qty), std::abs(prev_qty));
    const double avg_entry = prev_cost / static_cast<double>(prev_qty);
    const int direction = prev_qty > 0 ? 1 : -1;
    realized_pnl_ += (fill_px - avg_entry) * static_cast<double>(closing_qty) * direction;
    new_qty = prev_qty + signed_qty;

    if (new_qty == 0) {
      cost_basis_[symbol] = 0.0;
    } else if ((new_qty > 0) == (prev_qty > 0)) {
      // Partial close: what is left was bought at the original average.
      cost_basis_[symbol] = avg_entry * static_cast<double>(new_qty);
    } else {
      // The order was large enough to flip the position. The residual is a
      // brand-new position opened at *this* fill price -- carrying the old
      // average forward would attribute PnL that was already realized above
      // to the new position and double-count it.
      //
      // NOTE: hft/execution/paper.py originally carried the old average
      // forward here. That was a real bug (no Python test covered a flip);
      // it has been fixed on both sides so the two implementations agree.
      cost_basis_[symbol] = fill_px * static_cast<double>(new_qty);
    }
  }

  realized_pnl_ -= fee;
  fees_paid_ += fee;
  positions_[symbol] = new_qty;
}

Fill PaperVenue::submit(const Order& order, Price reference_price) {
  Quantity filled_qty = 0;
  const Price fill_px_ticks = fill_price_for(order, reference_price, filled_qty);

  // Work in currency units from here so fees and PnL are not quantised to ticks.
  const double fill_px = price_to_double(fill_px_ticks);
  const double fee = fill_px * static_cast<double>(filled_qty) * (cfg_.fee_bps / 10'000.0);

  if (filled_qty > 0) apply_pnl(order.symbol, order.side, filled_qty, fill_px, fee);
  ++fill_count_;

  Fill f{};
  f.order_id = order.id;
  f.symbol = order.symbol;
  f.side = order.side;
  f.price = fill_px_ticks;
  f.quantity = filled_qty;
  f.fee = fee;
  f.filled_ts_ns = now_ns();

  if (record_curve_) {
    curve_.push_back(PnlPoint{f.filled_ts_ns, fill_count_, realized_pnl_, fees_paid_,
                              fill_px_ticks, order.side, filled_qty, position(order.symbol)});
  }
  return f;
}

double PaperVenue::equity(SymbolId symbol, Price mark_price) const {
  const std::int64_t qty = position(symbol);
  if (qty == 0) return realized_pnl_;
  auto it = cost_basis_.find(symbol);
  const double cost = it == cost_basis_.end() ? 0.0 : it->second;
  const double unrealized = price_to_double(mark_price) * static_cast<double>(qty) - cost;
  return realized_pnl_ + unrealized;
}

bool PaperVenue::write_fills_csv(const std::string& path) const {
  std::ofstream f(path);
  if (!f) return false;
  f << "fill_index,ts_ns,side,fill_price,quantity,position,realized_pnl,fees_paid\n";
  for (const auto& p : curve_) {
    f << p.fill_index << ',' << p.ts_ns << ',' << to_string(p.side) << ','
      << price_to_double(p.fill_price) << ',' << p.quantity << ',' << p.position << ','
      << p.realized_pnl << ',' << p.fees_paid << '\n';
  }
  return static_cast<bool>(f);
}

}  // namespace hft
