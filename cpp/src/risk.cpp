#include "hft/risk.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace hft {

const char* to_string(RejectReason r) {
  switch (r) {
    case RejectReason::None: return "none";
    case RejectReason::KillSwitchEngaged: return "kill_switch_engaged";
    case RejectReason::InvalidOrder: return "invalid_order";
    case RejectReason::OrderQuantityTooLarge: return "order_quantity_too_large";
    case RejectReason::OrderNotionalTooLarge: return "order_notional_too_large";
    case RejectReason::PriceOutsideCollar: return "price_outside_collar";
    case RejectReason::PositionLimitExceeded: return "position_limit_exceeded";
    case RejectReason::GrossPositionLimitExceeded: return "gross_position_limit_exceeded";
    case RejectReason::ThrottleExceeded: return "throttle_exceeded";
    case RejectReason::DailyOrderLimitExceeded: return "daily_order_limit_exceeded";
    case RejectReason::DrawdownLimitExceeded: return "drawdown_limit_exceeded";
    case RejectReason::kCount: break;
  }
  return "unknown";
}

const char* to_string(HaltReason r) {
  switch (r) {
    case HaltReason::NotHalted: return "not_halted";
    case HaltReason::Manual: return "manual";
    case HaltReason::DrawdownBreach: return "drawdown_breach";
    case HaltReason::DailyOrderLimit: return "daily_order_limit";
    case HaltReason::OrderTimeout: return "order_timeout";
    case HaltReason::kCount: break;
  }
  return "unknown";
}

RiskManager::RiskManager(RiskLimits limits) : limits_(limits) { positions_.reserve(64); }

std::int64_t RiskManager::position(SymbolId symbol) const {
  return symbol < positions_.size() ? positions_[symbol] : 0;
}

void RiskManager::set_symbol_limit(SymbolId symbol, std::int64_t max_position) {
  if (symbol >= symbol_limits_.size()) symbol_limits_.resize(symbol + 1, 0);
  // Clamped, never trusted: an instrument may tighten the global limit but
  // must not be able to widen it.
  symbol_limits_[symbol] =
      max_position <= 0 ? 0 : std::min(max_position, limits_.max_position_per_symbol);
}

std::int64_t RiskManager::symbol_limit(SymbolId symbol) const {
  const std::int64_t specific = symbol < symbol_limits_.size() ? symbol_limits_[symbol] : 0;
  return specific > 0 ? specific : limits_.max_position_per_symbol;
}

RiskDecision RiskManager::reject(RejectReason reason) {
  ++reject_counts_[static_cast<std::size_t>(reason)];
  ++rejected_;
  return RiskDecision{false, reason};
}

RiskDecision RiskManager::check(const Order& order, Price reference_price, Nanos now_ns) {
  // Structural validity is checked even with limits disabled: a zero-quantity
  // order is a bug in every configuration.
  if (order.quantity <= 0) return reject(RejectReason::InvalidOrder);

  if (!limits_.enabled) {
    ++accepted_;
    ++daily_orders_;
    return RiskDecision{true, RejectReason::None};
  }

  // 1. Kill switch first: when halted, nothing else matters.
  if (halted()) return reject(RejectReason::KillSwitchEngaged);

  // 2. Fat-finger size checks -- cheapest, and catch the most catastrophic
  //    class of bug (a stray zero on a quantity).
  if (order.quantity > limits_.max_order_quantity) {
    return reject(RejectReason::OrderQuantityTooLarge);
  }

  const Price px = order.price > 0 ? order.price : reference_price;
  if (px <= 0) return reject(RejectReason::InvalidOrder);

  const double notional = price_to_double(px) * static_cast<double>(order.quantity);
  if (notional > limits_.max_order_notional) {
    return reject(RejectReason::OrderNotionalTooLarge);
  }

  // 3. Price collar. Guards against acting on a corrupt or stale price: if the
  //    order price is absurd relative to where the market actually is, the
  //    input is wrong, not the market.
  if (reference_price > 0 && limits_.price_collar_bps > 0.0) {
    const double ref = static_cast<double>(reference_price);
    const double deviation_bps = std::fabs(static_cast<double>(px) - ref) / ref * 10'000.0;
    if (deviation_bps > limits_.price_collar_bps) {
      return reject(RejectReason::PriceOutsideCollar);
    }
  }

  // 4. Inventory limits, evaluated on the position this order *would* create
  //    if it filled -- including everything already sent and not yet answered.
  //    Without the in-flight term a burst of orders can march straight past the
  //    position limit while every one of them is still in the air.
  const std::int64_t signed_qty =
      (order.side == Side::Buy) ? order.quantity : -order.quantity;
  const std::int64_t filled = position(order.symbol);
  const std::int64_t working = exposure_ != nullptr ? exposure_->working_exposure(order.symbol) : 0;
  const std::int64_t current = filled + working;
  const std::int64_t projected = current + signed_qty;

  if (std::llabs(projected) > symbol_limit(order.symbol)) {
    return reject(RejectReason::PositionLimitExceeded);
  }

  // Gross exposure across all symbols, with this symbol's contribution replaced
  // by its projected one.
  //
  // The other symbols' term is |filled| + |working| summed separately, which
  // over-states any symbol whose filled position and working orders point in
  // opposite directions. That is deliberate: the error is always toward
  // rejecting, and computing it exactly would mean walking every symbol on
  // every order -- work the pre-trade path must not do.
  const std::int64_t gross_working =
      exposure_ != nullptr ? exposure_->gross_working_exposure() : 0;
  const std::int64_t others =
      (gross_position_ - std::llabs(filled)) + (gross_working - std::llabs(working));
  const std::int64_t projected_gross = others + std::llabs(projected);
  if (projected_gross > limits_.max_gross_position) {
    return reject(RejectReason::GrossPositionLimitExceeded);
  }

  // 5. Rate limits. A runaway strategy loop is throttled here rather than
  //    being allowed to flood the venue.
  if (daily_orders_ >= limits_.max_daily_orders) {
    engage_kill_switch(HaltReason::DailyOrderLimit);
    return reject(RejectReason::DailyOrderLimitExceeded);
  }

  if (limits_.max_orders_per_second > 0) {
    if (window_start_ns_ == 0 || now_ns - window_start_ns_ >= 1'000'000'000) {
      window_start_ns_ = now_ns;
      window_orders_ = 0;
    }
    if (window_orders_ >= limits_.max_orders_per_second) {
      return reject(RejectReason::ThrottleExceeded);
    }
    ++window_orders_;
  }

  ++accepted_;
  ++daily_orders_;
  return RiskDecision{true, RejectReason::None};
}

void RiskManager::on_fill(const Fill& fill) {
  if (fill.quantity <= 0) return;
  if (fill.symbol >= positions_.size()) positions_.resize(fill.symbol + 1, 0);

  const std::int64_t signed_qty =
      (fill.side == Side::Buy) ? fill.quantity : -fill.quantity;
  const std::int64_t before = positions_[fill.symbol];
  const std::int64_t after = before + signed_qty;
  positions_[fill.symbol] = after;
  gross_position_ += std::llabs(after) - std::llabs(before);
}

void RiskManager::on_equity(double equity) {
  current_equity_ = equity;
  if (equity > peak_equity_) peak_equity_ = equity;

  if (!limits_.enabled || limits_.max_drawdown <= 0.0) return;
  if (peak_equity_ - equity > limits_.max_drawdown && !halted()) {
    // Sticky by design: a strategy that has already lost the day's budget does
    // not get to keep trading because the next tick happened to be favourable.
    engage_kill_switch(HaltReason::DrawdownBreach);
  }
}

void RiskManager::engage_kill_switch(HaltReason reason) {
  if (reason == HaltReason::NotHalted) return;
  if (halt_reason_ == HaltReason::NotHalted) halt_reason_ = reason;
}

void RiskManager::release_kill_switch() { halt_reason_ = HaltReason::NotHalted; }

void RiskManager::roll_day() {
  daily_orders_ = 0;
  window_start_ns_ = 0;
  window_orders_ = 0;
}

std::string RiskManager::summary() const {
  std::ostringstream os;
  char line[192];
  std::snprintf(line, sizeof(line),
                "risk: accepted=%llu rejected=%llu halted=%s gross=%lld peak_equity=%.2f "
                "drawdown=%.2f\n",
                (unsigned long long)accepted_, (unsigned long long)rejected_,
                to_string(halt_reason_), (long long)gross_position_, peak_equity_,
                drawdown());
  os << line;
  for (std::size_t i = 1; i < static_cast<std::size_t>(RejectReason::kCount); ++i) {
    if (reject_counts_[i] == 0) continue;
    std::snprintf(line, sizeof(line), "  reject %-30s %llu\n",
                  to_string(static_cast<RejectReason>(i)),
                  (unsigned long long)reject_counts_[i]);
    os << line;
  }
  return os.str();
}

}  // namespace hft
