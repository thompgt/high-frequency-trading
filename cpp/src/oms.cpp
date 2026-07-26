#include "hft/oms.hpp"

#include <algorithm>
#include <cstdio>

namespace hft {

const char* to_string(OrderState s) {
  switch (s) {
    case OrderState::PendingNew: return "PENDING_NEW";
    case OrderState::New: return "NEW";
    case OrderState::PartiallyFilled: return "PARTIALLY_FILLED";
    case OrderState::PendingCancel: return "PENDING_CANCEL";
    case OrderState::Filled: return "FILLED";
    case OrderState::Cancelled: return "CANCELLED";
    case OrderState::Rejected: return "REJECTED";
    case OrderState::Expired: return "EXPIRED";
    case OrderState::kCount: break;
  }
  return "UNKNOWN";
}

bool is_terminal(OrderState s) {
  return s == OrderState::Filled || s == OrderState::Cancelled ||
         s == OrderState::Rejected || s == OrderState::Expired;
}

bool is_working(OrderState s) { return !is_terminal(s); }

const char* to_string(ExecType t) {
  switch (t) {
    case ExecType::Acked: return "ACKED";
    case ExecType::VenueRejected: return "VENUE_REJECTED";
    case ExecType::Fill: return "FILL";
    case ExecType::Cancelled: return "CANCELLED";
    case ExecType::CancelRejected: return "CANCEL_REJECTED";
    case ExecType::kCount: break;
  }
  return "UNKNOWN";
}

OrderManager::OrderManager() : OrderManager(Config()) {}

OrderManager::OrderManager(Config config) : cfg_(config) {
  if (cfg_.max_open_orders == 0) cfg_.max_open_orders = 1;
  if (cfg_.retired_history == 0) cfg_.retired_history = 1;
  if (cfg_.ack_timeout_ns < 0) cfg_.ack_timeout_ns = 0;

  // Room for the working set plus everything we keep queryable after it
  // finishes. Reserved up front so steady-state operation never allocates.
  const std::size_t capacity = cfg_.max_open_orders + cfg_.retired_history;
  pool_.resize(capacity);
  free_slots_.reserve(capacity);
  for (std::size_t i = capacity; i-- > 0;) {
    free_slots_.push_back(static_cast<std::uint32_t>(i));
  }
  index_.reserve(capacity * 2);
  retired_.assign(cfg_.retired_history, 0);
}

std::uint32_t OrderManager::slot_for(ClOrdId id) const {
  const auto it = index_.find(id);
  return it == index_.end() ? kNilSlot : it->second;
}

std::uint32_t OrderManager::alloc_slot() {
  if (free_slots_.empty()) return kNilSlot;
  const std::uint32_t slot = free_slots_.back();
  free_slots_.pop_back();
  return slot;
}

OrderManager::Exposure& OrderManager::exposure_for(SymbolId symbol) {
  if (symbol >= exposure_.size()) exposure_.resize(static_cast<std::size_t>(symbol) + 1);
  return exposure_[symbol];
}

void OrderManager::add_exposure(SymbolId symbol, Side side, Quantity delta) {
  if (delta == 0) return;
  Exposure& e = exposure_for(symbol);
  // Gross is maintained incrementally: back out this symbol's old contribution,
  // apply the change, add the new one. Recomputing would mean walking every
  // symbol on every order, which is exactly what the risk path must not do.
  const std::int64_t before = std::abs(e.buy - e.sell);
  if (side == Side::Buy) {
    e.buy += delta;
    if (e.buy < 0) e.buy = 0;
  } else {
    e.sell += delta;
    if (e.sell < 0) e.sell = 0;
  }
  gross_exposure_ += std::abs(e.buy - e.sell) - before;
}

ClOrdId OrderManager::create(const Order& order, Nanos now_ns) {
  return create_with_id(next_id_, order, now_ns);
}

void OrderManager::set_next_id(ClOrdId next) {
  // Only ever forward. Rewinding would reissue ids that are already on the
  // wire and in the journal.
  if (next > next_id_) next_id_ = next;
}

ClOrdId OrderManager::create_with_id(ClOrdId id, const Order& order, Nanos now_ns) {
  if (id == 0 || slot_for(id) != kNilSlot) {
    // Id zero is the "no order" sentinel, and a duplicate id would silently
    // merge two distinct orders into one record.
    ++stats_.capacity_rejects;
    return 0;
  }
  if (order.quantity <= 0) {
    // A zero or negative order is not an order. It never reaches the venue and
    // it never gets an id -- the caller's `if (id == 0)` branch handles it.
    ++stats_.capacity_rejects;
    return 0;
  }
  if (open_count_ >= cfg_.max_open_orders) {
    ++stats_.capacity_rejects;
    return 0;
  }
  std::uint32_t slot = alloc_slot();
  if (slot == kNilSlot) {
    // Every slot is held by a retired record. Evict the oldest to make room:
    // remembering history must never cost us the ability to trade.
    if (retired_size_ == 0) {
      ++stats_.capacity_rejects;
      return 0;
    }
    const std::size_t oldest =
        (retired_cursor_ + retired_.size() - retired_size_) % retired_.size();
    const ClOrdId evicted = retired_[oldest];
    const std::uint32_t evicted_slot = slot_for(evicted);
    if (evicted_slot == kNilSlot) {
      ++stats_.capacity_rejects;
      return 0;
    }
    index_.erase(evicted);
    --retired_size_;
    slot = evicted_slot;
  }

  if (id >= next_id_) next_id_ = id + 1;
  OrderRecord& rec = pool_[slot];
  rec = OrderRecord{};
  rec.cl_ord_id = id;
  rec.symbol = order.symbol;
  rec.side = order.side;
  rec.type = order.type;
  rec.price = order.price;
  rec.quantity = order.quantity;
  rec.leaves = order.quantity;
  rec.state = OrderState::PendingNew;
  rec.created_ts_ns = now_ns;
  rec.last_update_ts_ns = now_ns;

  index_[id] = slot;
  ++open_count_;
  add_exposure(rec.symbol, rec.side, rec.quantity);
  ++stats_.created;
  return id;
}

ClOrdId OrderManager::adopt(const OrderRecord& snapshot, Nanos now_ns) {
  if (is_terminal(snapshot.state)) {
    // A finished order has no leaves and nothing to cancel. Tracking it would
    // put exposure in the book that cannot ever be worked off.
    ++stats_.capacity_rejects;
    return 0;
  }
  Order order;
  order.symbol = snapshot.symbol;
  order.side = snapshot.side;
  order.type = snapshot.type;
  order.price = snapshot.price;
  order.quantity = snapshot.quantity;
  order.created_ts_ns = now_ns;
  const ClOrdId id = create_with_id(snapshot.cl_ord_id, order, now_ns);
  if (id == 0) return 0;  // create_with_id has already counted the reason

  OrderRecord& rec = pool_[slot_for(id)];
  // create_with_id booked the full quantity as working, which is right for a
  // new order and wrong for a partly-filled one. Correct it to what the venue
  // says is actually left.
  Quantity leaves = snapshot.leaves;
  if (leaves < 0) leaves = 0;
  if (leaves > rec.quantity) leaves = rec.quantity;
  add_exposure(rec.symbol, rec.side, leaves - rec.leaves);
  rec.leaves = leaves;
  rec.filled = std::min(snapshot.filled < 0 ? 0 : snapshot.filled, rec.quantity);
  rec.venue_order_id = snapshot.venue_order_id;
  rec.avg_fill_price = snapshot.avg_fill_price;
  rec.fees = snapshot.fees;
  rec.state = snapshot.state;
  rec.created_ts_ns = snapshot.created_ts_ns != 0 ? snapshot.created_ts_ns : now_ns;
  // An adopted order is live at the venue by definition, so it is acked as far
  // as the timeout sweep is concerned -- expiring it would be claiming it never
  // arrived when we have just been told it did.
  rec.acked_ts_ns = now_ns;
  rec.last_update_ts_ns = now_ns;

  // It was not created here; it was inherited. Keeping the two apart is what
  // lets `created` stay a count of orders this session actually sent.
  --stats_.created;
  ++stats_.adopted;
  return id;
}

void OrderManager::retire(std::uint32_t slot) {
  OrderRecord& rec = pool_[slot];
  // Whatever was still working is not working any more.
  if (rec.leaves > 0) {
    add_exposure(rec.symbol, rec.side, -rec.leaves);
    rec.leaves = 0;
  }
  if (open_count_ > 0) --open_count_;

  // Push onto the retired ring, forgetting the oldest if it is full.
  if (retired_size_ == retired_.size()) {
    const ClOrdId evicted = retired_[retired_cursor_];
    const std::uint32_t evicted_slot = slot_for(evicted);
    if (evicted_slot != kNilSlot) {
      index_.erase(evicted);
      free_slots_.push_back(evicted_slot);
    }
    --retired_size_;
  }
  retired_[retired_cursor_] = rec.cl_ord_id;
  retired_cursor_ = (retired_cursor_ + 1) % retired_.size();
  ++retired_size_;
}

bool OrderManager::apply(const ExecutionReport& report) {
  const std::uint32_t slot = slot_for(report.cl_ord_id);
  if (slot == kNilSlot) {
    // An id we once issued but have since forgotten is a late report; an id we
    // never issued is a genuine break with the venue.
    if (report.cl_ord_id != 0 && report.cl_ord_id < next_id_) {
      ++stats_.late_reports;
    } else {
      ++stats_.unknown_reports;
    }
    return false;
  }

  OrderRecord& rec = pool_[slot];
  const OrderState state = rec.state;
  if (report.venue_order_id != 0) rec.venue_order_id = report.venue_order_id;

  switch (report.type) {
    case ExecType::Acked: {
      if (state != OrderState::PendingNew) {
        ++stats_.invalid_transitions;
        return false;
      }
      rec.state = OrderState::New;
      rec.acked_ts_ns = report.ts_ns;
      rec.last_update_ts_ns = report.ts_ns;
      ++stats_.acked;
      return true;
    }

    case ExecType::VenueRejected: {
      // A venue only rejects an order it has not accepted.
      if (state != OrderState::PendingNew) {
        ++stats_.invalid_transitions;
        return false;
      }
      rec.state = OrderState::Rejected;
      rec.last_update_ts_ns = report.ts_ns;
      ++stats_.venue_rejected;
      retire(slot);
      return true;
    }

    case ExecType::Fill: {
      // A fill is legal in every non-terminal state. Venues do fill before the
      // ack lands, and they do fill an order in the same instant we cancel it;
      // refusing either would put our position out of step with reality.
      if (is_terminal(state)) {
        ++stats_.invalid_transitions;
        return false;
      }
      if (report.quantity <= 0) {
        ++stats_.invalid_transitions;
        return false;
      }

      const Quantity q = report.quantity;
      if (q > rec.leaves) {
        // The venue executed more than we asked for. This is recorded in full
        // and flagged, never clamped: we are on the hook for the whole
        // quantity whatever our own arithmetic says, and a position that
        // understates reality is the dangerous direction to be wrong in.
        ++stats_.overfills;
      }

      const Quantity consumed = std::min(q, rec.leaves);
      const double px = price_to_double(report.price);
      const double prior = static_cast<double>(rec.filled);
      rec.avg_fill_price =
          (rec.avg_fill_price * prior + px * static_cast<double>(q)) / (prior + static_cast<double>(q));
      rec.filled += q;
      rec.leaves = rec.quantity > rec.filled ? rec.quantity - rec.filled : 0;
      rec.fees += report.fee;
      rec.last_update_ts_ns = report.ts_ns;
      add_exposure(rec.symbol, rec.side, -consumed);
      ++stats_.fills;

      if (rec.leaves == 0) {
        rec.state = OrderState::Filled;
        ++stats_.filled_orders;
        retire(slot);
      } else if (state != OrderState::PendingCancel) {
        // Staying in PendingCancel matters: the cancel is still outstanding and
        // we are still expecting an answer to it.
        rec.state = OrderState::PartiallyFilled;
      }
      return true;
    }

    case ExecType::Cancelled: {
      // Both solicited (we asked) and unsolicited (cancel-on-disconnect, an
      // expiring day order) cancels arrive here.
      if (state != OrderState::PendingCancel && state != OrderState::New &&
          state != OrderState::PartiallyFilled) {
        ++stats_.invalid_transitions;
        return false;
      }
      rec.state = OrderState::Cancelled;
      rec.last_update_ts_ns = report.ts_ns;
      ++stats_.cancelled;
      retire(slot);
      return true;
    }

    case ExecType::CancelRejected: {
      // The cancel did not take; the order is still live and still ours.
      if (state != OrderState::PendingCancel) {
        ++stats_.invalid_transitions;
        return false;
      }
      rec.state = rec.filled > 0 ? OrderState::PartiallyFilled : OrderState::New;
      rec.last_update_ts_ns = report.ts_ns;
      ++stats_.cancel_rejected;
      return true;
    }

    case ExecType::kCount: break;
  }
  ++stats_.invalid_transitions;
  return false;
}

bool OrderManager::request_cancel(ClOrdId id, Nanos now_ns) {
  const std::uint32_t slot = slot_for(id);
  if (slot == kNilSlot) return false;
  OrderRecord& rec = pool_[slot];
  // PendingNew is deliberately not cancellable: the venue has not told us the
  // order exists, so there is nothing there to cancel yet. Wait for the ack or
  // for the timeout to classify it.
  if (rec.state != OrderState::New && rec.state != OrderState::PartiallyFilled) return false;
  rec.state = OrderState::PendingCancel;
  rec.last_update_ts_ns = now_ns;
  ++stats_.cancels_requested;
  return true;
}

std::size_t OrderManager::sweep_timeouts(Nanos now_ns, std::vector<ClOrdId>* out) {
  std::size_t expired = 0;
  // Walk the index rather than the pool: the working set is small and the pool
  // is mostly retired records.
  std::vector<ClOrdId> to_expire;
  for (const auto& kv : index_) {
    const OrderRecord& rec = pool_[kv.second];
    if (rec.state != OrderState::PendingNew) continue;
    if (now_ns - rec.created_ts_ns < cfg_.ack_timeout_ns) continue;
    to_expire.push_back(rec.cl_ord_id);
  }

  for (const ClOrdId id : to_expire) {
    const std::uint32_t slot = slot_for(id);
    if (slot == kNilSlot) continue;
    OrderRecord& rec = pool_[slot];
    rec.state = OrderState::Expired;
    rec.last_update_ts_ns = now_ns;
    ++stats_.expired;
    retire(slot);
    if (out != nullptr) out->push_back(id);
    ++expired;
  }
  return expired;
}

const OrderRecord* OrderManager::find(ClOrdId id) const {
  const std::uint32_t slot = slot_for(id);
  return slot == kNilSlot ? nullptr : &pool_[slot];
}

std::vector<ClOrdId> OrderManager::open_orders() const {
  std::vector<ClOrdId> ids;
  ids.reserve(open_count_);
  for (const auto& kv : index_) {
    if (is_working(pool_[kv.second].state)) ids.push_back(kv.first);
  }
  // Deterministic order: the hash map's iteration order is not something a
  // caller (or a test, or a log line) should depend on.
  std::sort(ids.begin(), ids.end());
  return ids;
}

Quantity OrderManager::working_quantity(SymbolId symbol, Side side) const {
  if (symbol >= exposure_.size()) return 0;
  const Exposure& e = exposure_[symbol];
  return side == Side::Buy ? e.buy : e.sell;
}

std::int64_t OrderManager::working_exposure(SymbolId symbol) const {
  if (symbol >= exposure_.size()) return 0;
  const Exposure& e = exposure_[symbol];
  return e.buy - e.sell;
}

std::int64_t OrderManager::gross_working_exposure() const { return gross_exposure_; }

void OrderManager::clear() {
  const std::size_t capacity = pool_.size();
  pool_.assign(capacity, OrderRecord{});
  free_slots_.clear();
  for (std::size_t i = capacity; i-- > 0;) {
    free_slots_.push_back(static_cast<std::uint32_t>(i));
  }
  index_.clear();
  open_count_ = 0;
  next_id_ = 1;
  exposure_.clear();
  gross_exposure_ = 0;
  retired_.assign(cfg_.retired_history, 0);
  retired_cursor_ = 0;
  retired_size_ = 0;
  stats_ = OmsStats{};
}

std::string OrderManager::summary() const {
  char buf[1024];
  std::snprintf(buf, sizeof(buf),
                "orders created      : %llu\n"
                "  acked             : %llu\n"
                "  fills             : %llu  (%llu order(s) fully filled)\n"
                "  cancelled         : %llu  (%llu requested, %llu cancel rejects)\n"
                "  venue rejected    : %llu\n"
                "  expired (no ack)  : %llu\n"
                "adopted from venue  : %llu\n"
                "working orders      : %zu\n"
                "gross working qty   : %lld\n"
                "reconciliation breaks: %llu"
                "  (unknown %llu, invalid %llu, overfill %llu, capacity %llu, late %llu)\n",
                (unsigned long long)stats_.created, (unsigned long long)stats_.acked,
                (unsigned long long)stats_.fills, (unsigned long long)stats_.filled_orders,
                (unsigned long long)stats_.cancelled, (unsigned long long)stats_.cancels_requested,
                (unsigned long long)stats_.cancel_rejected,
                (unsigned long long)stats_.venue_rejected, (unsigned long long)stats_.expired,
                (unsigned long long)stats_.adopted, open_count_, (long long)gross_exposure_, (unsigned long long)stats_.breaks(),
                (unsigned long long)stats_.unknown_reports,
                (unsigned long long)stats_.invalid_transitions,
                (unsigned long long)stats_.overfills, (unsigned long long)stats_.capacity_rejects,
                (unsigned long long)stats_.late_reports);
  return std::string(buf);
}

}  // namespace hft
