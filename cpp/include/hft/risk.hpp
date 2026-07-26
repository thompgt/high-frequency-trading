// Pre-trade risk controls.
//
// Every order the strategy produces passes through RiskManager::check() before
// it can reach the venue. An engine that can emit orders without limits is
// dangerous by construction: a strategy bug, a corrupt price, or a feed glitch
// turns directly into unbounded loss. This layer is the thing that stops that,
// so it is deliberately simple, allocation-free, and fully unit-tested -- it
// must be the most boring code in the repository.
//
// Design rules:
//   * Fail closed. Anything the manager cannot evaluate is a rejection.
//   * Every rejection carries a machine-readable reason and is counted, so
//     "why did we stop trading?" is answerable from the metrics dump alone.
//   * Checks are ordered cheapest-and-most-fatal first.
//   * No exceptions and no allocation on the check path.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "hft/types.hpp"

namespace hft {

enum class RejectReason : std::uint8_t {
  None = 0,
  KillSwitchEngaged,
  InvalidOrder,             // non-positive quantity, or unusable price
  OrderQuantityTooLarge,    // fat finger, share count
  OrderNotionalTooLarge,    // fat finger, currency value
  PriceOutsideCollar,       // price is absurd relative to the reference
  PositionLimitExceeded,    // per-symbol
  GrossPositionLimitExceeded,
  ThrottleExceeded,         // orders per second
  DailyOrderLimitExceeded,
  DrawdownLimitExceeded,
  kCount
};

const char* to_string(RejectReason r);

enum class HaltReason : std::uint8_t {
  NotHalted = 0,
  Manual,
  DrawdownBreach,
  DailyOrderLimit,
  kCount
};

const char* to_string(HaltReason r);

struct RiskLimits {
  // Fat-finger bounds on a single order.
  Quantity max_order_quantity = 10'000;
  double max_order_notional = 1'000'000.0;  // currency units

  // Inventory bounds.
  std::int64_t max_position_per_symbol = 50'000;
  std::int64_t max_gross_position = 200'000;  // sum of |position| across symbols

  // Price sanity: reject orders more than this far from the reference price.
  double price_collar_bps = 500.0;  // 5%

  // Rate limits.
  std::uint32_t max_orders_per_second = 1000;
  std::uint64_t max_daily_orders = 1'000'000;

  // Loss control. Measured as peak-to-trough equity; breaching it engages the
  // kill switch, which is sticky until explicitly reset.
  double max_drawdown = 25'000.0;

  // Set false to run without limits. Present only so the limits can be
  // disabled explicitly and visibly rather than by setting every bound to a
  // huge number and hoping.
  bool enabled = true;
};

struct RiskDecision {
  bool accepted = false;
  RejectReason reason = RejectReason::None;

  explicit operator bool() const { return accepted; }
};

class RiskManager {
 public:
  explicit RiskManager(RiskLimits limits);

  // The pre-trade gate. `reference_price` is the engine's current fair-value
  // estimate (book mid where available) and is what the collar is measured
  // against. `now_ns` is injected so tests can drive the throttle
  // deterministically instead of sleeping.
  RiskDecision check(const Order& order, Price reference_price, Nanos now_ns);

  // Call after every fill so inventory and drawdown stay current. Breaching
  // the drawdown limit engages the kill switch from here.
  void on_fill(const Fill& fill);

  // Marks equity for drawdown tracking. Equity is realized + unrealized PnL.
  void on_equity(double equity);

  // --- kill switch ---------------------------------------------------------
  void engage_kill_switch(HaltReason reason);
  void release_kill_switch();  // deliberately manual: a tripped limit stays tripped
  bool halted() const { return halt_reason_ != HaltReason::NotHalted; }
  HaltReason halt_reason() const { return halt_reason_; }

  // True when the engine should flatten inventory rather than keep quoting.
  bool should_flatten() const { return halted() && limits_.enabled; }

  // --- state ---------------------------------------------------------------
  std::int64_t position(SymbolId symbol) const;
  std::int64_t gross_position() const { return gross_position_; }
  double peak_equity() const { return peak_equity_; }
  double current_equity() const { return current_equity_; }
  double drawdown() const { return peak_equity_ - current_equity_; }
  std::uint64_t accepted_orders() const { return accepted_; }
  std::uint64_t rejected_orders() const { return rejected_; }
  std::uint64_t daily_orders() const { return daily_orders_; }
  std::uint64_t rejects(RejectReason r) const {
    return reject_counts_[static_cast<std::size_t>(r)];
  }
  const RiskLimits& limits() const { return limits_; }

  // Resets the per-day counters (order count, throttle window). Does NOT clear
  // the kill switch or positions -- restarting the day must be a decision, not
  // a side effect.
  void roll_day();

  std::string summary() const;

 private:
  RiskDecision reject(RejectReason reason);

  RiskLimits limits_;
  std::vector<std::int64_t> positions_;  // indexed by SymbolId, grown on demand
  std::int64_t gross_position_ = 0;

  double peak_equity_ = 0.0;
  double current_equity_ = 0.0;

  // Fixed-window throttle: cheaper than a sliding window and, for a 1-second
  // bucket, close enough. Documented rather than hidden: a burst straddling a
  // window boundary can briefly exceed the nominal rate.
  Nanos window_start_ns_ = 0;
  std::uint32_t window_orders_ = 0;

  std::uint64_t daily_orders_ = 0;
  std::uint64_t accepted_ = 0;
  std::uint64_t rejected_ = 0;
  HaltReason halt_reason_ = HaltReason::NotHalted;

  std::array<std::uint64_t, static_cast<std::size_t>(RejectReason::kCount)> reject_counts_{};
};

}  // namespace hft
