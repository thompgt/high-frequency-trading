// Order management: the lifecycle of an order between "we decided to trade"
// and "the venue is done with it".
//
// Why this layer exists
// ---------------------
// The paper venue fills synchronously: submit() takes an Order and hands back a
// Fill. Nothing real behaves that way. A real venue acknowledges an order some
// microseconds later, may reject it, may fill it in pieces over seconds, may
// never answer at all, and will happily fill an order in the same instant you
// asked to cancel it. An engine that models execution as a function call has no
// answer to the only question that matters during an incident: *what do we have
// working right now?*
//
// So orders live here, in an explicit state machine, from creation to a
// terminal state. Three things fall out of that, all of which are required to
// operate an engine rather than demo one:
//
//   1. **In-flight exposure.** Position alone is not risk. An engine that has
//      sent 500 lots and had none of them acked yet is carrying 500 lots of
//      risk that no position-based limit can see. `working_quantity()` is what
//      the pre-trade gate adds to the position so a burst of orders cannot
//      march past the position limit while every one of them is in flight.
//   2. **Timeouts.** An order with no response is the worst state to be in:
//      it may be live at the venue or may have been dropped on the wire, and
//      those demand opposite actions. `sweep_timeouts()` makes the ambiguity
//      explicit and countable instead of leaving the order pending forever.
//   3. **Reconciliation.** Every report that does not fit the state machine --
//      unknown id, duplicate, illegal transition, a fill larger than the order
//      -- is counted rather than swallowed. A non-zero count is a break with
//      the venue, and finding out at 09:31 beats finding out at settlement.
//
// Conventions
// -----------
//  * The engine owns client order ids (`ClOrdId`), assigned densely from 1.
//    The venue's own id is stored alongside but is never the key.
//  * No allocation on the hot path: records live in a slab with a free list,
//    and per-symbol exposure is maintained incrementally.
//  * Nothing here throws. Every abnormal input is a counted, named outcome.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "hft/risk.hpp"
#include "hft/types.hpp"

namespace hft {

using ClOrdId = std::uint64_t;

// The lifecycle. Terminal states are Filled, Cancelled, Rejected and Expired;
// once an order reaches one it never leaves it.
enum class OrderState : std::uint8_t {
  PendingNew = 0,   // sent to the venue, no response yet
  New,              // acknowledged and live
  PartiallyFilled,  // acknowledged and partly done
  PendingCancel,    // cancel sent, not yet confirmed
  Filled,
  Cancelled,
  Rejected,         // the venue refused it
  Expired,          // no response within the ack timeout -- state unknown
  kCount
};

const char* to_string(OrderState s);
bool is_terminal(OrderState s);
// True while the order can still trade, i.e. it counts toward working exposure.
bool is_working(OrderState s);

// What the venue is telling us. One report, one event.
enum class ExecType : std::uint8_t {
  Acked = 0,       // order accepted, now live
  VenueRejected,   // order refused outright
  Fill,            // (partial or complete) execution
  Cancelled,       // cancel confirmed
  CancelRejected,  // cancel refused; the order is still live
  kCount
};

const char* to_string(ExecType t);

struct ExecutionReport {
  ClOrdId cl_ord_id = 0;
  OrderId venue_order_id = 0;
  SymbolId symbol = 0;
  Side side = Side::Buy;
  ExecType type = ExecType::Acked;
  Price price = 0;         // fill price; ignored unless type == Fill
  Quantity quantity = 0;   // quantity of *this* event, not cumulative
  double fee = 0.0;
  Nanos ts_ns = 0;
};

struct OrderRecord {
  ClOrdId cl_ord_id = 0;
  OrderId venue_order_id = 0;
  SymbolId symbol = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  Price price = 0;
  Quantity quantity = 0;  // original order quantity
  Quantity filled = 0;    // cumulative executed
  Quantity leaves = 0;    // quantity that can still trade
  double avg_fill_price = 0.0;  // in currency units, not ticks
  double fees = 0.0;
  OrderState state = OrderState::PendingNew;
  Nanos created_ts_ns = 0;
  Nanos acked_ts_ns = 0;
  Nanos last_update_ts_ns = 0;
};

// Every way a report can fail to make sense. All are counted; none are fatal.
struct OmsStats {
  std::uint64_t created = 0;
  std::uint64_t acked = 0;
  std::uint64_t fills = 0;
  std::uint64_t filled_orders = 0;
  std::uint64_t cancels_requested = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t cancel_rejected = 0;
  std::uint64_t venue_rejected = 0;
  std::uint64_t expired = 0;
  // Orders taken over from a venue snapshot rather than originated here.
  std::uint64_t adopted = 0;

  // --- reconciliation breaks ---
  std::uint64_t unknown_reports = 0;      // no such order, and not recently retired
  std::uint64_t late_reports = 0;         // for an order we already retired
  std::uint64_t invalid_transitions = 0;  // e.g. a fill on a cancelled order
  std::uint64_t overfills = 0;            // venue filled more than we asked for
  std::uint64_t capacity_rejects = 0;     // no slot to track the order in

  // Sum of the four break counters. Non-zero means our view and the venue's
  // view have diverged and someone needs to look.
  std::uint64_t breaks() const {
    return unknown_reports + invalid_transitions + overfills + capacity_rejects;
  }
};

class OrderManager : public ExposureSource {
 public:
  struct Config {
    // How long an order may sit unacknowledged before we declare its state
    // unknown. One second is an eternity at this scale; it is deliberately set
    // well beyond any healthy round trip so that tripping it is meaningful.
    Nanos ack_timeout_ns = 1'000'000'000;

    // Hard ceiling on simultaneously working orders. Reaching it is a
    // rejection, not a resize: an engine that has run away and opened a
    // million orders must be stopped, not accommodated.
    std::size_t max_open_orders = 4096;

    // How many finished orders to keep queryable. A venue can send a report
    // after an order is done (a late fill, a duplicate ack), and an operator
    // asking "what happened to order 812?" needs an answer. Beyond this many,
    // the oldest are forgotten and further reports for them are counted as
    // late rather than unknown -- a distinction worth keeping, because an
    // unknown id means the venue invented an order and a late one only means
    // we outran our own memory.
    std::size_t retired_history = 8192;
  };

  OrderManager();  // default limits
  explicit OrderManager(Config config);

  // Registers `order` as PendingNew and returns its client order id, or 0 if
  // there is no capacity. Zero means *do not send the order*: an order we
  // cannot track is an order we cannot cancel, reconcile, or bound.
  ClOrdId create(const Order& order, Nanos now_ns);

  // Registers an order under an id we already know. Used by journal recovery,
  // which must reconstruct the exact ids the previous session used rather than
  // re-deriving them -- and by anything else adopting an order it did not
  // originate. Returns 0 if the id is zero, already tracked, or there is no
  // capacity.
  ClOrdId create_with_id(ClOrdId id, const Order& order, Nanos now_ns);

  // Takes responsibility for an order that already exists at the venue, in the
  // state the venue says it is in rather than as PendingNew. This is what a
  // restart does with the orders that startup reconciliation confirmed are
  // still resting: until they are here, they are live risk that no limit can
  // see and no cancel can reach.
  //
  // `snapshot` must carry a non-zero id, a positive quantity and a non-terminal
  // state; anything else returns 0 and is counted as a capacity reject, because
  // adopting a finished order would track exposure that does not exist.
  ClOrdId adopt(const OrderRecord& snapshot, Nanos now_ns);

  // Sets the id the next create() will hand out. Client order ids must be
  // unique across restarts -- a venue that sees the same id twice in a session
  // rejects it, and a journal with repeated ids cannot be replayed at all --
  // so a restart continues the sequence rather than beginning again at 1.
  // Ignored if it would move the sequence backwards.
  void set_next_id(ClOrdId next);
  ClOrdId next_id() const { return next_id_; }

  // Applies a venue report to the state machine. Returns false if the report
  // was not applied; the reason is reflected in the counters.
  bool apply(const ExecutionReport& report);

  // Moves a live order to PendingCancel. Returns false if the order is not in
  // a cancellable state.
  bool request_cancel(ClOrdId id, Nanos now_ns);

  // Moves every order that has been PendingNew for longer than the ack timeout
  // into Expired, appending their ids to `out`. Returns how many expired.
  std::size_t sweep_timeouts(Nanos now_ns, std::vector<ClOrdId>* out = nullptr);

  // --- queries -------------------------------------------------------------

  const OrderRecord* find(ClOrdId id) const;
  std::size_t open_count() const { return open_count_; }
  std::vector<ClOrdId> open_orders() const;

  // Quantity still able to trade on one side of one symbol. This is the number
  // pre-trade risk must add to the position.
  Quantity working_quantity(SymbolId symbol, Side side) const;

  // Signed working exposure: buy leaves minus sell leaves. Adding this to the
  // current position gives the position we would hold if everything in flight
  // filled -- the quantity a position limit actually needs to bound.
  std::int64_t working_exposure(SymbolId symbol) const override;

  // Sum of |working_exposure| across symbols.
  std::int64_t gross_working_exposure() const override;

  const OmsStats& stats() const { return stats_; }
  const Config& config() const { return cfg_; }
  std::string summary() const;

  // Drops all state. For tests and for a deliberate start-of-day reset; never
  // call it to "fix" a break, which only hides the divergence.
  void clear();

 private:
  static constexpr std::uint32_t kNilSlot = 0xFFFFFFFFu;

  std::uint32_t slot_for(ClOrdId id) const;
  std::uint32_t alloc_slot();
  // Moves a finished order out of the working set: releases its remaining
  // exposure and pushes it into the retired ring, evicting (and only then
  // forgetting) the oldest.
  void retire(std::uint32_t slot);

  // Exposure is maintained incrementally rather than recomputed, so a risk
  // check never walks the open-order set.
  void add_exposure(SymbolId symbol, Side side, Quantity delta);

  struct Exposure {
    Quantity buy = 0;
    Quantity sell = 0;
  };
  Exposure& exposure_for(SymbolId symbol);

  Config cfg_;
  std::vector<OrderRecord> pool_;
  std::vector<std::uint32_t> free_slots_;
  std::unordered_map<ClOrdId, std::uint32_t> index_;
  std::size_t open_count_ = 0;
  ClOrdId next_id_ = 1;

  std::vector<Exposure> exposure_;  // indexed by SymbolId, grown on demand
  std::int64_t gross_exposure_ = 0;

  // Circular buffer of finished order ids, newest last. Records stay in
  // `pool_` and `index_` until they fall out of here.
  std::vector<ClOrdId> retired_;
  std::size_t retired_cursor_ = 0;
  std::size_t retired_size_ = 0;

  OmsStats stats_;
};

}  // namespace hft
