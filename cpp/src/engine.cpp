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
      risk_(config.risk),
      oms_(config.oms),
      ring_(config.ring_capacity) {
  trade_scratch_.reserve(256);
  venue_.set_record_curve(config.record_curve);
  // The pre-trade gate must see sent-but-unfilled quantity, not just filled
  // position -- otherwise a burst of in-flight orders walks past the limit.
  risk_.set_exposure_source(&oms_);
}

Fill Engine::dispatch(const Order& order, Price reference_price, ClOrdId cl_ord_id,
                      EngineStats& stats) {
  const Fill fill = venue_.submit(order, reference_price);

  // Translate the venue's synchronous answer into the report sequence a real
  // venue would send. Doing it here rather than special-casing the OMS means
  // the state machine sees exactly the same event stream it would see from a
  // live session, and swapping PaperVenue for a real one changes nothing
  // downstream.
  ExecutionReport rep{};
  rep.cl_ord_id = cl_ord_id;
  rep.venue_order_id = fill.order_id;
  rep.symbol = order.symbol;
  rep.side = order.side;
  rep.ts_ns = fill.filled_ts_ns;

  rep.type = ExecType::Acked;
  oms_.apply(rep);

  if (fill.quantity > 0) {
    rep.type = ExecType::Fill;
    rep.price = fill.price;
    rep.quantity = fill.quantity;
    rep.fee = fill.fee;
    oms_.apply(rep);
  }

  // A marketable order does not rest, so any remainder is dead as soon as the
  // venue answers. Reporting it as cancelled is what the venue would do for an
  // IOC, and it is what stops the unfilled part sitting in the OMS as
  // permanent phantom exposure.
  const OrderRecord* rec = oms_.find(cl_ord_id);
  if (rec != nullptr && is_working(rec->state)) {
    if (fill.quantity > 0) ++stats.partial_fills;
    rep.type = ExecType::Cancelled;
    rep.quantity = 0;
    rep.fee = 0.0;
    oms_.apply(rep);
  }
  return fill;
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
  } else if (last_reference_ > 0) {
    reference = last_reference_;
  } else {
    return;  // one-sided empty book and no usable price on this message
  }
  last_reference_ = reference;

  // --- stage 3: strategy ---------------------------------------------------
  Signal signal{};
  const Nanos sig_start = now_ns();
  const bool fired = strategy_.on_tick(tick, reference, signal);
  const Nanos sig_end = now_ns();
  latency_.signal_compute.record(sig_end - sig_start);
  if (!fired) return;
  ++stats.signals;

  // --- stage 4: pre-trade risk ---------------------------------------------
  Order order{};
  order.id = next_order_id_++;
  order.symbol = signal.symbol;
  order.side = signal.side;
  order.type = OrderType::Market;
  order.price = signal.price;
  order.quantity = cfg_.order_quantity;
  order.created_ts_ns = sig_end;

  const Nanos risk_start = now_ns();
  const RiskDecision decision = risk_.check(order, reference, risk_start);
  latency_.risk_check.record(now_ns() - risk_start);

  if (!decision) {
    // Rejections are counted by reason inside RiskManager; the engine only
    // needs the aggregate. Nothing is sent, and the run continues -- a
    // rejected order is a normal outcome, not a fatal error.
    ++stats.risk_rejects;
    return;
  }

  // --- stage 5: execution --------------------------------------------------
  const Nanos ord_start = now_ns();
  const ClOrdId cl_ord_id = oms_.create(order, ord_start);
  if (cl_ord_id == 0) {
    // No room to track it. An order we cannot track is one we cannot cancel or
    // reconcile, so it does not go out -- refusing to trade is always
    // recoverable, losing track of a live order is not.
    ++stats.untracked_rejects;
    return;
  }
  const Fill fill = dispatch(order, signal.price, cl_ord_id, stats);
  const Nanos ord_end = now_ns();

  latency_.order_round_trip.record(ord_end - ord_start);
  latency_.tick_to_order.record(ord_end - tick.ingest_ts_ns);
  ++stats.orders_sent;

  // --- stage 6: post-trade risk --------------------------------------------
  risk_.on_fill(fill);
  risk_.on_equity(venue_.equity(order.symbol, reference));
  if (risk_.halted()) stats.halted = true;
}

std::uint64_t Engine::flatten(EngineStats& stats) {
  std::uint64_t sent = 0;
  double mid = 0.0;
  const bool have_mid = book_.mid_price(mid);

  // Snapshot first: submitting mutates the venue's position map.
  std::vector<std::pair<SymbolId, std::int64_t>> open;
  for (const auto& kv : venue_.positions()) {
    if (kv.second != 0) open.emplace_back(kv.first, kv.second);
  }

  for (const auto& kv : open) {
    Order order{};
    order.id = next_order_id_++;
    order.symbol = kv.first;
    order.side = kv.second > 0 ? Side::Sell : Side::Buy;
    order.type = OrderType::Market;
    order.quantity = kv.second > 0 ? kv.second : -kv.second;
    order.created_ts_ns = now_ns();

    // Mark the exit at the book mid, or the last fair value we saw. Flattening
    // against a price of zero would silently book a fictional loss.
    const Price reference = have_mid ? price_from_double(mid) : last_reference_;
    if (reference <= 0) continue;  // never seen a usable price: nothing sane to do

    // Flattening bypasses the pre-trade gate (see the header) but not the OMS:
    // an exit order is still an order, and losing track of it would leave us
    // unsure whether the position is actually closed.
    const ClOrdId cl_ord_id = oms_.create(order, order.created_ts_ns);
    if (cl_ord_id == 0) {
      ++stats.untracked_rejects;
      continue;
    }
    const Fill fill = dispatch(order, reference, cl_ord_id, stats);
    risk_.on_fill(fill);
    ++sent;
    ++stats.flatten_orders;
  }
  return sent;
}

EngineStats Engine::run(MarketDataSource& feed) {
  return cfg_.threaded ? run_threaded(feed) : run_inline(feed);
}

EngineStats Engine::run_inline(MarketDataSource& feed) {
  EngineStats stats;
  Tick tick;
  const Nanos t0 = now_ns();
  while (!stop_requested() && feed.next(tick)) process(tick, stats);
  stats.wall_ns = now_ns() - t0;
  return stats;
}

EngineStats Engine::run_threaded(MarketDataSource& feed) {
  EngineStats stats;
  std::atomic<bool> producer_done{false};
  const Nanos t0 = now_ns();

  std::thread producer([&] {
    Tick tick;
    while (!stop_requested() && feed.next(tick)) {
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
    if (stop_requested() && producer_done.load(std::memory_order_acquire)) break;
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
