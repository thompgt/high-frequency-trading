#include "hft/engine.hpp"

#include <atomic>
#include <fstream>
#include <thread>

#include "hft/log.hpp"

namespace hft {
namespace {
// Ticks between clock reads in the run loops. Reading the clock is far more
// expensive than the counter that guards it, and the sweep itself only needs
// to be roughly periodic -- the ack timeout is measured in whole seconds.
constexpr std::uint64_t kSweepCheckTicks = 1024;
}  // namespace

Engine::Engine(EngineConfig config)
    : cfg_(config),
      instruments_(config.instruments),
      strategy_(config.fast_window, config.slow_window),
      venue_(PaperVenue::Config{config.slippage_bps, config.fee_bps, nullptr}),
      risk_(config.risk),
      oms_(config.oms),
      feed_(config.feed_health),
      ring_(config.ring_capacity) {
  // No instruments configured means the single-symbol case: synthesise one
  // from the price band so every existing configuration keeps working and the
  // common setup stays a one-liner.
  if (instruments_.empty()) {
    Instrument only;
    only.symbol = "DEFAULT";
    only.min_price = config.min_price;
    only.max_price = config.max_price;
    std::string error;
    if (!instruments_.add(only, error)) {
      throw std::invalid_argument("invalid default instrument: " + error);
    }
  }

  books_.reserve(instruments_.size());
  for (const auto& instrument : instruments_.all()) {
    books_.push_back(std::make_unique<OrderBook>(instrument.min_price, instrument.max_price));
    // A per-instrument ceiling may only tighten the global one. Enforced here
    // rather than trusted from config, so a mistake cannot widen a limit.
    if (instrument.max_position > 0) {
      risk_.set_symbol_limit(instrument.id, instrument.max_position);
    }
  }

  if (config.cross_book) venue_.set_books(&book_provider_);

  trade_scratch_.reserve(256);
  venue_.set_record_curve(config.record_curve);
  // The pre-trade gate must see sent-but-unfilled quantity, not just filled
  // position -- otherwise a burst of in-flight orders walks past the limit.
  risk_.set_exposure_source(&oms_);
}

OrderBook* Engine::Books::book_for(SymbolId symbol) {
  return symbol < engine_->books_.size() ? engine_->books_[symbol].get() : nullptr;
}

OrderBook& Engine::book(SymbolId symbol) {
  if (symbol >= books_.size()) {
    throw std::out_of_range("no book for symbol " + std::to_string(symbol));
  }
  return *books_[symbol];
}

const OrderBook& Engine::book(SymbolId symbol) const {
  if (symbol >= books_.size()) {
    throw std::out_of_range("no book for symbol " + std::to_string(symbol));
  }
  return *books_[symbol];
}

void Engine::emit(const ExecutionReport& report, EngineStats& stats) {
  if (journal_ != nullptr && !journal_->record_exec_report(report)) {
    ++stats.journal_failures;
    // Losing the ability to record what happened is not a degraded mode, it is
    // a stop condition: from here on a crash would leave state we cannot
    // reconstruct.
    risk_.engage_kill_switch(HaltReason::Manual);
    request_stop();
  }
  oms_.apply(report);
}

Fill Engine::dispatch(const Order& order, Price reference_price, ClOrdId cl_ord_id,
                      EngineStats& stats) {
  // Record the intent *before* the order can exist at the venue. The reverse
  // order has a window in which we are filled on something the journal never
  // heard of.
  if (journal_ != nullptr && !journal_->record_order_sent(cl_ord_id, order, order.created_ts_ns)) {
    ++stats.journal_failures;
    risk_.engage_kill_switch(HaltReason::Manual);
    request_stop();
  }

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
  emit(rep, stats);

  if (fill.quantity > 0) {
    rep.type = ExecType::Fill;
    rep.price = fill.price;
    rep.quantity = fill.quantity;
    rep.fee = fill.fee;
    emit(rep, stats);
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
    emit(rep, stats);
  }
  return fill;
}

void Engine::process(const Tick& tick, EngineStats& stats) {
  ++stats.ticks;

  // --- stage 0: is this message usable at all? -----------------------------
  // Everything downstream assumes the book reflects the exchange's book, and
  // that assumption is only as good as the message stream that built it.
  const FeedStatus status = feed_.on_tick(tick, tick.ingest_ts_ns);
  if (status == FeedStatus::Duplicate || status == FeedStatus::Reordered) {
    // A duplicate would double-apply an add; a reordered message would undo
    // newer state. Both corrupt the book, so neither is applied.
    ++stats.stale_messages;
    return;
  }
  if (feed_.faulted()) {
    // We know the book is missing updates. It will keep accepting messages so
    // the depth stays roughly current for the operator's benefit, but nothing
    // trades off it until the session is explicitly resynchronised.
    if (!risk_.halted()) {
      risk_.engage_kill_switch(HaltReason::Manual);
      if (journal_ != nullptr) {
        journal_->record_halt(static_cast<std::uint8_t>(HaltReason::Manual), tick.ingest_ts_ns);
      }
    }
    stats.halted = true;
    ++stats.ticks_while_faulted;
  }

  // --- stage 0b: is this message for something we trade? -------------------
  const Instrument* instrument = instruments_.find(tick.symbol);
  if (instrument == nullptr) {
    // A feed sending a symbol we never subscribed to is a configuration or
    // entitlement problem. Guessing a price band for it would be worse than
    // dropping it.
    ++stats.unknown_symbol_ticks;
    return;
  }
  OrderBook& book_ = *books_[tick.symbol];

  // Contract validation. A price off the tick grid or a size off the lot size
  // means either the feed is wrong or our instrument definition is; either way
  // applying it would build a book the exchange does not have.
  const bool needs_price = tick.type == TickType::AddOrder;
  if (needs_price && (!instrument->price_is_valid(tick.price) ||
                      !instrument->quantity_is_valid(tick.quantity))) {
    ++stats.contract_violations;
    return;
  }

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
  // Fair value is per instrument -- carrying one across symbols would price
  // one product off another's book.
  if (tick.symbol >= last_reference_.size()) {
    last_reference_.resize(static_cast<std::size_t>(tick.symbol) + 1, 0);
  }
  Price reference;
  double mid = 0.0;
  if (book_.mid_price(mid)) {
    reference = price_from_double(mid);
  } else if (tick.price > 0) {
    reference = tick.price;
  } else if (last_reference_[tick.symbol] > 0) {
    reference = last_reference_[tick.symbol];
  } else {
    return;  // one-sided empty book and no usable price on this message
  }
  last_reference_[tick.symbol] = reference;

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
  // Snap to the instrument's contract before the order can reach a venue that
  // would reject it. Rounding is toward the passive side, so it never pushes
  // the order further into the market than the strategy asked for.
  order.price = instrument->round_price(signal.price, signal.side);
  order.quantity = cfg_.order_quantity;
  if (instrument->lot_size > 1) {
    order.quantity -= order.quantity % instrument->lot_size;
    if (order.quantity <= 0) {
      // The configured order size is smaller than one lot, so there is no
      // valid order to send. Counted, not silently rounded up to a lot.
      ++stats.contract_violations;
      return;
    }
  }
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
  const bool was_halted = risk_.halted();
  risk_.on_fill(fill);
  risk_.on_equity(venue_.equity(order.symbol, reference));
  if (risk_.halted()) {
    stats.halted = true;
    // Why we stopped is the first question anyone asks afterwards, so the
    // transition is journalled (and fsynced) rather than merely counted.
    if (!was_halted && journal_ != nullptr) {
      if (!journal_->record_halt(static_cast<std::uint8_t>(risk_.halt_reason()), ord_end)) {
        ++stats.journal_failures;
      }
    }
  }
}

std::uint64_t Engine::flatten(EngineStats& stats) {
  std::uint64_t sent = 0;

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

    // Mark the exit at *this instrument's* book mid, or the last fair value we
    // saw for it. Flattening against a price of zero, or against another
    // product's price, would silently book a fictional PnL.
    double mid = 0.0;
    const bool have_mid = kv.first < books_.size() && books_[kv.first]->mid_price(mid);
    const Price fallback =
        kv.first < last_reference_.size() ? last_reference_[kv.first] : 0;
    const Price reference = have_mid ? price_from_double(mid) : fallback;
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

std::size_t Engine::sweep_orders(Nanos now, EngineStats& stats, bool force) {
  if (!force) {
    if (last_sweep_ns_ == 0) {
      // First call: start the clock rather than immediately sweeping, so an
      // order sent in the first microsecond of a run is not judged against a
      // timeout that began at the epoch.
      last_sweep_ns_ = now;
      return 0;
    }
    if (now - last_sweep_ns_ < cfg_.order_sweep_interval_ns) return 0;
  }
  last_sweep_ns_ = now;

  expired_scratch_.clear();
  const std::size_t expired = oms_.sweep_timeouts(now, &expired_scratch_);
  if (expired == 0) return 0;

  stats.timed_out_orders += expired;
  for (const ClOrdId id : expired_scratch_) {
    // Logged individually and at error level: each one is an order whose state
    // at the venue is unknown, and the ids are what an operator needs to query
    // it with.
    HFT_ERROR("order timed out with no acknowledgement", "cl_ord_id",
              static_cast<double>(id));
  }

  // An unanswered order may be resting at the venue or may have died on the
  // wire, and those demand opposite actions. Neither is something to keep
  // quoting through, so by default this stops trading and lets a human decide.
  if (cfg_.halt_on_order_timeout && !risk_.halted()) {
    risk_.engage_kill_switch(HaltReason::OrderTimeout);
    stats.halted = true;
    if (journal_ != nullptr) {
      journal_->record_halt(static_cast<std::uint8_t>(HaltReason::OrderTimeout), now);
    }
  }
  return expired;
}

EngineStats Engine::run(MarketDataSource& feed) {
  return cfg_.threaded ? run_threaded(feed) : run_inline(feed);
}

EngineStats Engine::run_inline(MarketDataSource& feed) {
  EngineStats stats;
  Tick tick;
  const Nanos t0 = now_ns();
  last_sweep_ns_ = t0;
  std::uint64_t since_sweep = 0;
  while (!stop_requested() && feed.next(tick)) {
    process(tick, stats);
    // Checking the clock every tick would cost more than the sweep it guards,
    // so the counter gates the clock read and the clock gates the sweep.
    if (++since_sweep >= kSweepCheckTicks) {
      since_sweep = 0;
      sweep_orders(now_ns(), stats);
    }
  }
  sweep_orders(now_ns(), stats, /*force=*/true);
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

  last_sweep_ns_ = t0;
  std::uint64_t since_sweep = 0;
  Tick tick;
  for (;;) {
    if (ring_.try_pop(tick)) {
      process(tick, stats);
      // A busy consumer may never reach the idle branch below, so the sweep
      // cannot live there alone.
      if (++since_sweep >= kSweepCheckTicks) {
        since_sweep = 0;
        sweep_orders(now_ns(), stats);
      }
      continue;
    }
    if (producer_done.load(std::memory_order_acquire) && ring_.empty_approx()) break;
    if (stop_requested() && producer_done.load(std::memory_order_acquire)) break;

    // Idle is the only place a silent feed can be noticed: a feed that has
    // stopped sending will never call process() to say so. A frozen book and a
    // quiet market are indistinguishable from inside, and only one of them is
    // safe to quote against.
    const Nanos idle_now = now_ns();
    if (feed_.check_stale(idle_now) && !risk_.halted()) {
      risk_.engage_kill_switch(HaltReason::Manual);
      stats.halted = true;
      if (journal_ != nullptr) {
        journal_->record_halt(static_cast<std::uint8_t>(HaltReason::Manual), idle_now);
      }
    }
    // Idle is also where an unanswered order is most likely to be noticed: a
    // venue that has stopped responding is also a venue sending us nothing.
    sweep_orders(idle_now, stats);
    std::this_thread::yield();
  }

  sweep_orders(now_ns(), stats, /*force=*/true);
  producer.join();
  stats.wall_ns = now_ns() - t0;
  stats.dropped_ticks = ring_.dropped();
  return stats;
}

bool Engine::write_book_snapshot_csv(const std::string& path, std::size_t levels) const {
  std::ofstream f(path);
  if (!f) return false;
  f << "symbol,side,price,quantity,order_count\n";
  for (const auto& instrument : instruments_.all()) {
    const OrderBook& book = *books_[instrument.id];
    for (const auto& lv : book.depth(Side::Buy, levels)) {
      f << instrument.symbol << ",BID," << price_to_double(lv.price) << ',' << lv.quantity << ','
        << lv.order_count << '\n';
    }
    for (const auto& lv : book.depth(Side::Sell, levels)) {
      f << instrument.symbol << ",ASK," << price_to_double(lv.price) << ',' << lv.quantity << ','
        << lv.order_count << '\n';
    }
  }
  return static_cast<bool>(f);
}

}  // namespace hft
