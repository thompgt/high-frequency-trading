// Strategy interface + the moving-average crossover port.
//
// Same contract as hft/core/strategy.py: given a tick, optionally produce a
// Signal. The engine knows nothing else about the strategy.
//
// One deliberate difference from the Python version: strategy.py recomputes
// both averages with sum() on every tick, which is O(slow_window) per tick.
// Here both averages are maintained as running sums over a fixed ring, so
// on_tick is O(1) regardless of window size. The signals produced are
// identical (see cpp/tests -- the crossover semantics, including "the first
// computed state never fires", are preserved exactly).
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "hft/types.hpp"

namespace hft {

class Strategy {
 public:
  virtual ~Strategy() = default;
  // Returns true and fills `out` when the tick triggers a signal.
  virtual bool on_tick(const Tick& tick, Price reference_price, Signal& out) = 0;
  virtual void reset() {}
  virtual const char* name() const = 0;
};

class MovingAverageCrossover : public Strategy {
 public:
  MovingAverageCrossover(std::size_t fast_window, std::size_t slow_window);

  bool on_tick(const Tick& tick, Price reference_price, Signal& out) override;
  void reset() override;
  const char* name() const override { return "MovingAverageCrossover"; }

  std::size_t fast_window() const { return fast_; }
  std::size_t slow_window() const { return slow_; }

  // Exposed for tests and for the notebook's signal-overlay chart.
  bool averages(SymbolId symbol, double& fast_avg, double& slow_avg) const;
  std::uint64_t signals_emitted() const { return signals_; }

 private:
  // Per-symbol rolling window. `ring` holds the last `slow_` prices; the fast
  // sum is over the most recent `fast_` of them.
  struct State {
    std::vector<Price> ring;
    std::size_t head = 0;    // next write slot
    std::size_t filled = 0;  // how many slots are populated
    std::int64_t slow_sum = 0;
    std::int64_t fast_sum = 0;
    bool has_state = false;  // has a previous fast_above been computed?
    bool fast_above = false;
  };

  State& state_for(SymbolId symbol);

  std::size_t fast_;
  std::size_t slow_;
  std::unordered_map<SymbolId, State> states_;
  std::uint64_t signals_ = 0;
};

}  // namespace hft
