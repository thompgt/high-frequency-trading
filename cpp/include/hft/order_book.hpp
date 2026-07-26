// Price-time-priority limit order book.
//
// This is the piece with no Python counterpart -- the Python pipeline only ever
// saw a last-trade price. A real trading engine needs the book itself.
//
// Data structure
// --------------
// The naive choice is std::map<Price, Level>, which costs a pointer-chasing
// O(log n) tree walk (and a cache miss per node) on every single add, cancel
// and match. Instead this book uses the layout production venues use:
//
//   1. A flat array of price levels covering a fixed price band. Converting a
//      price to a level is `price - min_price` -- one subtraction, no search,
//      and adjacent levels are adjacent in memory so walking depth is a
//      sequential scan the prefetcher handles for free.
//   2. Each level holds an intrusive FIFO doubly-linked list of orders, which
//      is what gives *time* priority within a price. Orders live in a slab
//      (`std::vector<OrderNode>`) with a free list, so add/cancel never
//      allocate after warm-up and links are 32-bit indices rather than 64-bit
//      pointers.
//   3. A three-level hierarchical bitmap tracks which levels are non-empty, so
//      "what is the best bid?" is a handful of count-leading-zeros
//      instructions rather than a scan over an empty price band. This is the
//      part that keeps best-bid/ask O(1) even after a level empties out.
//
// The trade-off is honest: the flat array costs memory proportional to the
// price band, not to the number of live levels, and prices outside the band are
// rejected. That is exactly the trade real books make -- instruments have known
// price bands and limit-up/limit-down bounds.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "hft/types.hpp"

namespace hft {

inline constexpr Price kNoPrice = std::numeric_limits<Price>::min();
inline constexpr std::uint32_t kNilNode = 0xFFFFFFFFu;

// Three-tier bitmap over N slots supporting O(1) find-highest / find-lowest.
// Tier 0 is one bit per slot, tier 1 one bit per tier-0 word, tier 2 one bit
// per tier-1 word. A 100k-level band is 1563 / 25 / 1 words respectively.
class LevelBitmap {
 public:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  explicit LevelBitmap(std::size_t n);

  void set(std::size_t i);
  void clear(std::size_t i);
  bool test(std::size_t i) const;
  bool empty() const { return l2_word_ == 0; }
  void reset();

  std::size_t find_highest() const;
  std::size_t find_lowest() const;

  // Exposed for the depth() walk, which steps level-by-level rather than
  // jumping straight to the extreme.
  const std::vector<std::uint64_t>& l0_words() const { return l0_; }
  std::size_t bit_count() const { return n_; }

 private:
  std::size_t n_ = 0;
  std::vector<std::uint64_t> l0_;
  std::vector<std::uint64_t> l1_;
  std::uint64_t l2_word_ = 0;  // one bit per l1_ word; caps the band at 64*64*64 = 262144 levels
};

// Self-match prevention. Exchanges require it and so does any firm running
// more than one strategy against the same book: trading with yourself pays the
// spread and the fees for no economic reason, and in most venues it is a
// compliance violation. Off by default so a book with no participant identity
// behaves exactly as before.
enum class SelfMatchPolicy : std::uint8_t {
  None = 0,        // no STP; an aggressor may trade against its own resting order
  CancelResting,   // drop the resting order, aggressor continues to the next
  CancelAggressor, // stop the aggressor; its remainder is not rested
};

class OrderBook {
 public:
  struct LevelView {
    Price price = 0;
    Quantity quantity = 0;
    std::uint32_t order_count = 0;
  };

  // Inclusive price band, in ticks. Orders outside it are rejected.
  OrderBook(Price min_price, Price max_price);

  // --- mutating operations -------------------------------------------------

  // Aggressive-then-rest limit order. Crosses against the opposite side while
  // it can, then rests any remainder. Trades are appended to `out` if given.
  // Returns the quantity filled immediately, or -1 if the order was rejected
  // (duplicate live id, price out of band, non-positive quantity).
  Quantity add_limit(OrderId id, Side side, Price price, Quantity qty,
                     std::vector<Trade>* out = nullptr, OwnerId owner = 0);

  // Marketable-to-any-price order. Never rests; unfilled remainder is dropped.
  // Returns quantity filled.
  Quantity execute_market(OrderId id, Side side, Quantity qty,
                          std::vector<Trade>* out = nullptr, OwnerId owner = 0);

  // Removes a resting order. Returns false if the id is not live.
  bool cancel(OrderId id);

  // Amend a resting order. A pure size *reduction* at the same price keeps the
  // order's place in the queue (this is what exchanges allow). Any price change
  // or size increase is a cancel/replace and loses time priority -- and, since
  // the new price may cross, can trade immediately.
  bool modify(OrderId id, Price new_price, Quantity new_qty,
              std::vector<Trade>* out = nullptr);

  void clear();

  void set_self_match_policy(SelfMatchPolicy p) { smp_ = p; }
  SelfMatchPolicy self_match_policy() const { return smp_; }
  // Number of resting orders cancelled, or aggressors stopped, by STP.
  std::uint64_t self_match_preventions() const { return smp_events_; }

  // --- read-only accessors -------------------------------------------------

  Price best_bid() const;  // kNoPrice when that side is empty
  Price best_ask() const;
  Quantity best_bid_quantity() const;
  Quantity best_ask_quantity() const;

  // kNoPrice-safe: returns false when either side is empty.
  bool spread(Price& out) const;
  bool mid_price(double& out) const;

  Quantity quantity_at(Side side, Price price) const;
  // Top `depth` levels of one side, best price first.
  std::vector<LevelView> depth(Side side, std::size_t depth) const;

  std::size_t live_order_count() const { return live_orders_; }
  bool has_order(OrderId id) const;
  Price min_price() const { return min_price_; }
  Price max_price() const { return max_price_; }

 private:
  struct OrderNode {
    OrderId id = 0;
    Price price = 0;
    Quantity quantity = 0;
    std::uint32_t prev = kNilNode;
    std::uint32_t next = kNilNode;
    OwnerId owner = 0;
    Side side = Side::Buy;
    bool live = false;
  };

  struct PriceLevel {
    Quantity total_quantity = 0;
    std::uint32_t order_count = 0;
    std::uint32_t head = kNilNode;  // oldest -- fills first
    std::uint32_t tail = kNilNode;  // newest
  };

  bool in_band(Price p) const { return p >= min_price_ && p <= max_price_; }
  std::size_t index_of(Price p) const {
    return static_cast<std::size_t>(p - min_price_);
  }
  Price price_at(std::size_t idx) const {
    return min_price_ + static_cast<Price>(idx);
  }

  std::vector<PriceLevel>& side_levels(Side s) {
    return s == Side::Buy ? bids_ : asks_;
  }
  const std::vector<PriceLevel>& side_levels(Side s) const {
    return s == Side::Buy ? bids_ : asks_;
  }
  LevelBitmap& side_bitmap(Side s) { return s == Side::Buy ? bid_map_ : ask_map_; }
  const LevelBitmap& side_bitmap(Side s) const {
    return s == Side::Buy ? bid_map_ : ask_map_;
  }

  std::uint32_t alloc_node();
  void free_node(std::uint32_t slot);
  void link_back(PriceLevel& level, std::uint32_t slot);
  void unlink(PriceLevel& level, std::uint32_t slot);

  // Consumes resting liquidity on `resting_side` up to `limit` (kNoPrice means
  // any price). Returns quantity filled and appends trades.
  Quantity match(OrderId aggressor_id, OwnerId aggressor_owner, Side aggressor_side,
                 Price limit, Quantity qty, std::vector<Trade>* out);

  // Order-id lookup. A hash map is the general answer; ids here are dense
  // (assigned by our own feed/strategy), so a direct-mapped slot table with a
  // linear-probe fallback would be faster still. Kept as a map for clarity --
  // it is not on the matching hot path, only on cancel/modify.
  std::uint32_t slot_for(OrderId id) const;

  Price min_price_;
  Price max_price_;
  std::vector<PriceLevel> bids_;
  std::vector<PriceLevel> asks_;
  LevelBitmap bid_map_;
  LevelBitmap ask_map_;

  SelfMatchPolicy smp_ = SelfMatchPolicy::None;
  std::uint64_t smp_events_ = 0;
  // Set by match() when CancelAggressor fires, so add_limit knows not to rest
  // the remainder of an order the policy just stopped.
  bool aggressor_stopped_ = false;

  std::vector<OrderNode> pool_;
  std::uint32_t free_head_ = kNilNode;
  std::size_t live_orders_ = 0;

  // open-addressed id -> pool slot index
  std::vector<OrderId> id_keys_;
  std::vector<std::uint32_t> id_vals_;
  std::size_t id_mask_ = 0;
  std::size_t id_count_ = 0;       // live entries
  std::size_t id_tombstones_ = 0;  // erased-but-still-probed slots
  void id_insert(OrderId id, std::uint32_t slot);
  void id_erase(OrderId id);
  void id_grow();
};

}  // namespace hft
