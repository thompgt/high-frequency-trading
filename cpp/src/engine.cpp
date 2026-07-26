#include "hft/engine.hpp"

#include <atomic>
#include <fstream>
#include <thread>

namespace hft {

Engine::Engine(EngineConfig config)
    : cfg_(config),
      book_(config.min_price, config.max_price),
      strategy_(config.fast_window, config.slow_window),
      venue_(PaperVenue::Config{config.slippage_bps, config.fee_bps,
                                config.cross_book ? &book_ : nullptr}),
      ring_(config.ring_capacity) {
  trade_scratch_.reserve(256);
  venue_.set_record_curve(config.record_curve);
}

void Engine::process(const Tick& tick, EngineStats& stats) {
  ++stats.ticks;

  // --- stage 1: apply the message to the book ------------------------------
  const Nanos book_start = now_ns();
  trade_scratch_.clear();
  switch (tick.type) {
    case TickType::AddOrder:
      if (book_.add_limit(tick.order_id, tick.side, tick.price, tick.quantity, &trade_scratch_) >= 0) {
        ++stats.adds;
      }
      break;
    case TickType::CancelOrder:
      if (book_.cancel(tick.order_id)) {
        ++stats.cancels;
      } else {
        // The order already traded away. Real feeds do this constantly; it is
        // not an error, and swallowing it silently is the correct behaviour.
        ++stats.cancel_rejected;
      }
      break;
    case TickType::Trade:
      book_.execute_market(tick.order_id, tick.side, tick.quantity, &trade_scratch_);
      ++stats.aggressive_orders;
      break;
    case TickType::Quote:
      break;
  }
  const Nanos book_end = now_ns();
  stats.book_trades += trade_scratch_.size();
  latency_.book_update.record(book_end - book_start);
  latency_.ingest_to_book.record(book_end - tick.ingest_ts_ns);

  // --- stage 2: derive a reference price -----------------------------------
  // The mid of the live book is a far better fair-value estimate than the last
  // trade print, which is why having a real book matters even for a strategy
  // this simple.
  Price reference;
  double mid = 0.0;
  if (book_.mid_price(mid)) {
    reference = price_from_double(mid);
  } else if (tick.price > 0) {
    reference = tick.price;
  } else {
    return;  // one-sided empty book and no usable price on this message
  }

  // --- stage 3: strategy ---------------------------------------------------
  Signal signal{};
  const Nanos sig_start = now_ns();
  const bool fired = strategy_.on_tick(tick, reference, signal);
  const Nanos sig_end = now_ns();
  latency_.signal_compute.record(sig_end - sig_start);
  if (!fired) return;
  ++stats.signals;

  // --- stage 4: execution --------------------------------------------------
  Order order{};
  order.id = next_order_id_++;
  order.symbol = signal.symbol;
  order.side = signal.side;
  order.type = OrderType::Market;
  order.price = signal.price;
  order.quantity = cfg_.order_quantity;
  order.created_ts_ns = sig_end;

  const Nanos ord_start = now_ns();
  const Fill fill = venue_.submit(order, signal.price);
  const Nanos ord_end = now_ns();
  (void)fill;

  latency_.order_round_trip.record(ord_end - ord_start);
  latency_.tick_to_order.record(ord_end - tick.ingest_ts_ns);
  ++stats.orders_sent;
}

EngineStats Engine::run(MarketDataSource& feed) {
  return cfg_.threaded ? run_threaded(feed) : run_inline(feed);
}

EngineStats Engine::run_inline(MarketDataSource& feed) {
  EngineStats stats;
  Tick tick;
  const Nanos t0 = now_ns();
  while (feed.next(tick)) process(tick, stats);
  stats.wall_ns = now_ns() - t0;
  return stats;
}

EngineStats Engine::run_threaded(MarketDataSource& feed) {
  EngineStats stats;
  std::atomic<bool> producer_done{false};
  const Nanos t0 = now_ns();

  std::thread producer([&] {
    Tick tick;
    while (feed.next(tick)) {
      // Spin until the consumer makes room. Dropping would be the other valid
      // policy (and the ring counts drops when try_push fails); here we would
      // rather not silently lose messages in the demo, so we back off instead.
      while (!ring_.try_push(tick)) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  Tick tick;
  for (;;) {
    if (ring_.try_pop(tick)) {
      process(tick, stats);
      continue;
    }
    if (producer_done.load(std::memory_order_acquire) && ring_.empty_approx()) break;
    std::this_thread::yield();
  }

  producer.join();
  stats.wall_ns = now_ns() - t0;
  stats.dropped_ticks = ring_.dropped();
  return stats;
}

bool Engine::write_book_snapshot_csv(const std::string& path, std::size_t levels) const {
  std::ofstream f(path);
  if (!f) return false;
  f << "side,price,quantity,order_count\n";
  for (const auto& lv : book_.depth(Side::Buy, levels)) {
    f << "BID," << price_to_double(lv.price) << ',' << lv.quantity << ',' << lv.order_count << '\n';
  }
  for (const auto& lv : book_.depth(Side::Sell, levels)) {
    f << "ASK," << price_to_double(lv.price) << ',' << lv.quantity << ',' << lv.order_count << '\n';
  }
  return static_cast<bool>(f);
}

}  // namespace hft
