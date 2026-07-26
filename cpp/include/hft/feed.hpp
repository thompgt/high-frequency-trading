// Market data source abstraction.
//
// Same contract as hft/data/base.py's MarketDataSource: everything downstream
// depends on this interface and nothing else, so a real exchange feed (ITCH,
// FIX, a vendor WebSocket) drops in without touching the book, the strategy or
// the venue.
//
// Both implementations here run with **no network access at all**, which is
// deliberate:
//   * SyntheticFeed generates a deterministic order-flow stream from a seeded
//     PRNG. Same seed => byte-identical stream => benchmarks and tests are
//     reproducible, which they cannot be against a live feed.
//   * CsvReplayFeed replays a captured file. This is how you regression-test a
//     strategy against a known market episode.
#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "hft/types.hpp"

namespace hft {

class MarketDataSource {
 public:
  virtual ~MarketDataSource() = default;

  // Fills `out` with the next tick. Returns false when the stream is
  // exhausted. Implementations must stamp out.ingest_ts_ns themselves so the
  // measured latency includes everything after the message was received.
  virtual bool next(Tick& out) = 0;

  virtual void reset() {}
  virtual const char* name() const = 0;
};

// Deterministic synthetic order flow around a random-walking fair value.
//
// The generated mix is roughly what a quiet equity book looks like: mostly
// passive adds near the touch, a steady stream of cancels of previously added
// orders, and occasional aggressive marketable orders that actually trade.
class SyntheticFeed : public MarketDataSource {
 public:
  struct Params {
    SymbolId symbol = 0;
    Price start_price = 10000;   // $100.00 in ticks
    Price tick_range = 40;       // how far from fair value orders are placed
    Quantity min_qty = 1;
    Quantity max_qty = 500;
    std::uint64_t seed = 0x5EEDC0DEULL;
    std::size_t total_events = 1'000'000;
    // Event mix, must sum to <= 100.
    int pct_add = 70;
    int pct_cancel = 22;
    int pct_trade = 8;
    // Fair value drifts by +/-1 tick with this probability per event.
    int pct_move = 12;
    Price min_price = 5000;
    Price max_price = 15000;
  };

  explicit SyntheticFeed(Params params);

  bool next(Tick& out) override;
  void reset() override;
  const char* name() const override { return "SyntheticFeed"; }

  Price fair_value() const { return fair_; }
  const Params& params() const { return params_; }

  // Writes `count` events to a CSV replay file that CsvReplayFeed can read.
  static bool write_replay_csv(const std::string& path, Params params, std::size_t count);

 private:
  std::uint64_t rng();
  std::uint64_t rng_range(std::uint64_t n) { return rng() % n; }

  Params params_;
  std::uint64_t state_;
  Price fair_;
  std::size_t emitted_ = 0;
  OrderId next_id_ = 1;
  std::vector<OrderId> live_ids_;  // pool of ids eligible to be cancelled
};

// Replays a CSV file produced by SyntheticFeed::write_replay_csv (or by any
// capture tool emitting the same columns).
//
// Columns: symbol,type,side,price,quantity,order_id,source_ts_ns,sequence
// The trailing sequence column is optional; captures written before it existed
// replay with sequence 0.
class CsvReplayFeed : public MarketDataSource {
 public:
  explicit CsvReplayFeed(const std::string& path);

  bool next(Tick& out) override;
  void reset() override;
  const char* name() const override { return "CsvReplayFeed"; }

  std::size_t size() const { return ticks_.size(); }
  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }

 private:
  std::string path_;
  std::vector<Tick> ticks_;
  std::size_t cursor_ = 0;
  bool ok_ = false;
  std::string error_;
};

}  // namespace hft
