#include "hft/strategy.hpp"

#include <stdexcept>

namespace hft {

MovingAverageCrossover::MovingAverageCrossover(std::size_t fast_window, std::size_t slow_window)
    : fast_(fast_window), slow_(slow_window) {
  if (fast_window == 0) throw std::invalid_argument("fast_window must be positive");
  if (fast_window >= slow_window) {
    throw std::invalid_argument("fast_window must be smaller than slow_window");
  }
}

MovingAverageCrossover::State& MovingAverageCrossover::state_for(SymbolId symbol) {
  auto it = states_.find(symbol);
  if (it != states_.end()) return it->second;
  State s;
  s.ring.assign(slow_, 0);
  return states_.emplace(symbol, std::move(s)).first->second;
}

bool MovingAverageCrossover::on_tick(const Tick& tick, Price reference_price, Signal& out) {
  State& s = state_for(tick.symbol);

  // --- O(1) window update: subtract what leaves, add what arrives ----------
  if (s.filled == slow_) {
    s.slow_sum -= s.ring[s.head];  // the slot about to be overwritten is the oldest
  }
  if (s.filled >= fast_) {
    const std::size_t out_idx = (s.head + slow_ - fast_) % slow_;
    s.fast_sum -= s.ring[out_idx];
  }

  s.ring[s.head] = reference_price;
  s.head = (s.head + 1) % slow_;
  s.slow_sum += reference_price;
  s.fast_sum += reference_price;
  if (s.filled < slow_) ++s.filled;

  // Same warm-up rule as the Python version: no signals until the slow window
  // is completely full.
  if (s.filled < slow_) return false;

  // fast_avg > slow_avg, done in integers: (fast_sum / fast) > (slow_sum / slow)
  // <=> fast_sum * slow > slow_sum * fast. Avoids any float comparison in the
  // decision, so the signal sequence is bit-for-bit reproducible.
  const bool fast_above = (s.fast_sum * static_cast<std::int64_t>(slow_)) >
                          (s.slow_sum * static_cast<std::int64_t>(fast_));

  const bool had_state = s.has_state;
  const bool prev = s.fast_above;
  s.has_state = true;
  s.fast_above = fast_above;

  // The very first computed state is a baseline, not a crossover -- matching
  // strategy.py, where `prev is None` returns no signal.
  if (!had_state || prev == fast_above) return false;

  out.symbol = tick.symbol;
  out.side = fast_above ? Side::Buy : Side::Sell;
  out.price = reference_price;
  out.tick_ingest_ts_ns = tick.ingest_ts_ns;
  ++signals_;
  return true;
}

void MovingAverageCrossover::reset() {
  states_.clear();
  signals_ = 0;
}

bool MovingAverageCrossover::averages(SymbolId symbol, double& fast_avg, double& slow_avg) const {
  auto it = states_.find(symbol);
  if (it == states_.end() || it->second.filled < slow_) return false;
  fast_avg = static_cast<double>(it->second.fast_sum) / static_cast<double>(fast_);
  slow_avg = static_cast<double>(it->second.slow_sum) / static_cast<double>(slow_);
  return true;
}

}  // namespace hft
