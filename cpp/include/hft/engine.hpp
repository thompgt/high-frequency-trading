// The engine: feed -> ring buffer -> order book -> strategy -> venue.
//
// This is the C++ equivalent of hft/core/engine.py plus hft/main.py's wiring,
// with the order book inserted into the path (Python had no book) and with a
// genuinely concurrent ingestion thread rather than an asyncio task.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "hft/execution.hpp"
#include "hft/feed.hpp"
#include "hft/feed_health.hpp"
#include "hft/instrument.hpp"
#include "hft/journal.hpp"
#include "hft/latency.hpp"
#include "hft/oms.hpp"
#include "hft/order_book.hpp"
#include "hft/ring_buffer.hpp"
#include "hft/risk.hpp"
#include "hft/strategy.hpp"
#include "hft/types.hpp"

namespace hft {

struct EngineConfig {
  std::size_t fast_window = 5;
  std::size_t slow_window = 20;
  Quantity order_quantity = 1;
  double slippage_bps = 1.0;
  double fee_bps = 0.5;
  // The instruments to trade, one book each. When empty the engine creates a
  // single instrument spanning [min_price, max_price] below, which is what
  // every single-symbol configuration gets and keeps that case a one-liner.
  InstrumentRegistry instruments;
  Price min_price = 5000;
  Price max_price = 15000;
  std::size_t ring_capacity = 1 << 16;
  // When true the feed runs on its own thread and hands ticks over through the
  // lock-free ring buffer. When false everything runs inline on one thread,
  // which is the right mode for benchmarking the pipeline itself without
  // cross-core hand-off noise.
  bool threaded = true;
  // Cross real book liquidity on execution instead of the flat-bps model.
  bool cross_book = true;
  bool record_curve = true;
  // Pre-trade risk limits. Every order goes through these before the venue.
  RiskLimits risk;
  // Order lifecycle tracking. Its exposure feeds the risk gate, so an order
  // that cannot be tracked is an order that does not get sent.
  OrderManager::Config oms;
  // Market data session validation. A gap means the book is missing updates
  // and is therefore wrong; by default that stops trading.
  FeedHealthConfig feed_health;
};

struct EngineStats {
  std::uint64_t ticks = 0;
  std::uint64_t adds = 0;
  std::uint64_t cancels = 0;
  std::uint64_t cancel_rejected = 0;  // order already traded away
  std::uint64_t aggressive_orders = 0;
  std::uint64_t book_trades = 0;
  std::uint64_t signals = 0;
  std::uint64_t orders_sent = 0;
  std::uint64_t dropped_ticks = 0;
  std::uint64_t risk_rejects = 0;
  std::uint64_t flatten_orders = 0;
  // Orders the OMS had no room to track, and which were therefore never sent.
  std::uint64_t untracked_rejects = 0;
  // Partially-filled marketable orders whose remainder was closed out. A
  // market order does not rest, so its unfilled remainder is dead the moment
  // the venue answers -- leaving it "working" would leak exposure forever.
  std::uint64_t partial_fills = 0;
  // Journal writes that failed. Any of these is fatal to the run: an engine
  // that cannot record what it did cannot be recovered after a crash, so it
  // halts rather than trading blind.
  std::uint64_t journal_failures = 0;
  // Messages dropped because the feed said they were stale or duplicated.
  // Applying either would corrupt the book.
  std::uint64_t stale_messages = 0;
  // Ticks not traded on because the feed session was faulted.
  std::uint64_t ticks_while_faulted = 0;
  // Messages for a symbol we have no instrument for. A feed sending us
  // something we never subscribed to is a configuration or entitlement
  // problem, not something to guess our way through.
  std::uint64_t unknown_symbol_ticks = 0;
  // Messages whose price or quantity violated the instrument's contract
  // (off the tick grid, off the lot size, outside the band).
  std::uint64_t contract_violations = 0;
  bool halted = false;
  Nanos wall_ns = 0;

  double ticks_per_second() const {
    return wall_ns > 0 ? static_cast<double>(ticks) * 1e9 / static_cast<double>(wall_ns) : 0.0;
  }
};

class Engine {
 public:
  explicit Engine(EngineConfig config);

  // Drains `feed` to exhaustion and returns run statistics.
  EngineStats run(MarketDataSource& feed);

  // Per-instrument book. Throws std::out_of_range for an unknown symbol --
  // a caller asking for a book that does not exist has a bug, and returning a
  // reference to something arbitrary would hide it.
  OrderBook& book(SymbolId symbol);
  const OrderBook& book(SymbolId symbol) const;
  // The first instrument's book. Convenience for the single-instrument case,
  // which is most configurations and every benchmark.
  OrderBook& book() { return book(0); }
  const OrderBook& book() const { return book(0); }
  std::size_t book_count() const { return books_.size(); }
  const InstrumentRegistry& instruments() const { return instruments_; }
  PaperVenue& venue() { return venue_; }
  const PaperVenue& venue() const { return venue_; }
  MovingAverageCrossover& strategy() { return strategy_; }
  RiskManager& risk() { return risk_; }
  const RiskManager& risk() const { return risk_; }
  OrderManager& oms() { return oms_; }
  const OrderManager& oms() const { return oms_; }
  FeedMonitor& feed_monitor() { return feed_; }
  const FeedMonitor& feed_monitor() const { return feed_; }
  LatencyRecorder& latency() { return latency_; }
  const LatencyRecorder& latency() const { return latency_; }
  const EngineConfig& config() const { return cfg_; }

  // Applies one tick through the whole pipeline. Public so tests can drive the
  // engine deterministically without a feed or a thread.
  void process(const Tick& tick, EngineStats& stats);

  // Attaches a journal. Not owned; must outlive the engine. Every order and
  // every execution report is written to it before being acted on, so a crash
  // leaves a record of what was actually in flight.
  void set_journal(Journal* journal) { journal_ = journal; }
  Journal* journal() { return journal_; }

  bool write_book_snapshot_csv(const std::string& path, std::size_t levels) const;

  // Closes out inventory. Used on shutdown and when the kill switch trips:
  // being halted means "stop taking risk", which includes the risk already on
  // the books. Flattening orders deliberately bypass the pre-trade gate --
  // otherwise the position limit that halted us would also block the exit.
  std::uint64_t flatten(EngineStats& stats);

  // Cooperative stop, safe to call from a signal handler's flag check.
  void request_stop() { stop_requested_.store(true, std::memory_order_relaxed); }
  bool stop_requested() const { return stop_requested_.load(std::memory_order_relaxed); }

 private:
  EngineStats run_inline(MarketDataSource& feed);
  EngineStats run_threaded(MarketDataSource& feed);

  // Sends an accepted order to the venue and drives the resulting execution
  // reports through the OMS. Returns the fill.
  Fill dispatch(const Order& order, Price reference_price, ClOrdId cl_ord_id,
                EngineStats& stats);

  // Applies a report to the OMS and writes it to the journal. Journalling
  // first: a report we have acted on but not recorded is invisible to
  // recovery, which is the one ordering that loses information.
  void emit(const ExecutionReport& report, EngineStats& stats);

  // Hands the venue the right book for each order's instrument.
  class Books : public BookProvider {
   public:
    explicit Books(Engine* engine) : engine_(engine) {}
    OrderBook* book_for(SymbolId symbol) override;

   private:
    Engine* engine_;
  };

  EngineConfig cfg_;
  InstrumentRegistry instruments_;
  // One book per instrument, indexed by SymbolId. Held by pointer because
  // OrderBook is neither copyable nor movable (it hands out interior
  // references) and the vector must not reallocate its elements.
  std::vector<std::unique_ptr<OrderBook>> books_;
  Books book_provider_{this};
  MovingAverageCrossover strategy_;
  PaperVenue venue_;
  RiskManager risk_;
  OrderManager oms_;
  FeedMonitor feed_;
  LatencyRecorder latency_;
  SpscRingBuffer<Tick> ring_;
  std::vector<Trade> trade_scratch_;  // reused so matching never allocates
  Journal* journal_ = nullptr;
  OrderId next_order_id_ = 1;
  // Last usable fair value per instrument, for flattening. Per symbol because
  // one instrument's price says nothing about another's.
  std::vector<Price> last_reference_;
  std::atomic<bool> stop_requested_{false};
};

}  // namespace hft
