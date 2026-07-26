#include "hft/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace hft {
namespace {

inline std::uint64_t bit(std::size_t i) { return 1ULL << (i & 63); }
inline std::size_t highest_bit(std::uint64_t w) {
  return 63u - static_cast<std::size_t>(__builtin_clzll(w));
}
inline std::size_t lowest_bit(std::uint64_t w) {
  return static_cast<std::size_t>(__builtin_ctzll(w));
}

// splitmix64 finaliser -- cheap, good avalanche, no multiply-heavy loop.
inline std::uint64_t mix(std::uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

constexpr OrderId kEmptyKey = 0;                                  // id 0 is reserved/invalid
constexpr OrderId kTombstone = std::numeric_limits<OrderId>::max();

}  // namespace

// ---------------------------------------------------------------- LevelBitmap

LevelBitmap::LevelBitmap(std::size_t n) : n_(n) {
  const std::size_t l0_words = (n + 63) / 64;
  const std::size_t l1_words = (l0_words + 63) / 64;
  if (l1_words > 64) {
    throw std::invalid_argument(
        "LevelBitmap: price band exceeds 262144 levels; widen the tick size or "
        "narrow the band");
  }
  l0_.assign(l0_words, 0);
  l1_.assign(l1_words, 0);
}

void LevelBitmap::set(std::size_t i) {
  const std::size_t w0 = i >> 6;
  l0_[w0] |= bit(i);
  const std::size_t w1 = w0 >> 6;
  l1_[w1] |= bit(w0);
  l2_word_ |= bit(w1);
}

void LevelBitmap::clear(std::size_t i) {
  const std::size_t w0 = i >> 6;
  l0_[w0] &= ~bit(i);
  if (l0_[w0] != 0) return;
  const std::size_t w1 = w0 >> 6;
  l1_[w1] &= ~bit(w0);
  if (l1_[w1] != 0) return;
  l2_word_ &= ~bit(w1);
}

bool LevelBitmap::test(std::size_t i) const {
  return (l0_[i >> 6] & bit(i)) != 0;
}

void LevelBitmap::reset() {
  std::fill(l0_.begin(), l0_.end(), 0ULL);
  std::fill(l1_.begin(), l1_.end(), 0ULL);
  l2_word_ = 0;
}

std::size_t LevelBitmap::find_highest() const {
  if (l2_word_ == 0) return npos;
  const std::size_t w1 = highest_bit(l2_word_);
  const std::size_t w0 = (w1 << 6) + highest_bit(l1_[w1]);
  return (w0 << 6) + highest_bit(l0_[w0]);
}

std::size_t LevelBitmap::find_lowest() const {
  if (l2_word_ == 0) return npos;
  const std::size_t w1 = lowest_bit(l2_word_);
  const std::size_t w0 = (w1 << 6) + lowest_bit(l1_[w1]);
  return (w0 << 6) + lowest_bit(l0_[w0]);
}

namespace {

// Walk to the next set bit strictly above / below `i`. Used only by depth()
// traversal, which is a reporting path, not the matching hot path.
std::size_t scan_above(const std::vector<std::uint64_t>& l0, std::size_t n, std::size_t i) {
  if (i + 1 >= n) return LevelBitmap::npos;
  std::size_t j = i + 1;
  std::size_t w = j >> 6;
  std::uint64_t bits = l0[w] & (~0ULL << (j & 63));
  for (;;) {
    if (bits) return (w << 6) + lowest_bit(bits);
    if (++w >= l0.size()) return LevelBitmap::npos;
    bits = l0[w];
  }
}

std::size_t scan_below(const std::vector<std::uint64_t>& l0, std::size_t i) {
  if (i == 0) return LevelBitmap::npos;
  const std::size_t j = i - 1;
  std::size_t w = j >> 6;
  const std::size_t b = j & 63;
  std::uint64_t bits = l0[w] & (b == 63 ? ~0ULL : ((1ULL << (b + 1)) - 1));
  for (;;) {
    if (bits) return (w << 6) + highest_bit(bits);
    if (w == 0) return LevelBitmap::npos;
    bits = l0[--w];
  }
}

}  // namespace

// ------------------------------------------------------------------ OrderBook

OrderBook::OrderBook(Price min_price, Price max_price)
    : min_price_(min_price),
      max_price_(max_price),
      bids_(static_cast<std::size_t>(max_price - min_price + 1)),
      asks_(static_cast<std::size_t>(max_price - min_price + 1)),
      bid_map_(static_cast<std::size_t>(max_price - min_price + 1)),
      ask_map_(static_cast<std::size_t>(max_price - min_price + 1)) {
  if (max_price < min_price) {
    throw std::invalid_argument("OrderBook: max_price < min_price");
  }
  pool_.reserve(1024);
  id_keys_.assign(1024, kEmptyKey);
  id_vals_.assign(1024, kNilNode);
  id_mask_ = id_keys_.size() - 1;
}

// --- id index (open addressing, linear probe, tombstone deletion) ------------

std::uint32_t OrderBook::slot_for(OrderId id) const {
  if (id == kEmptyKey || id == kTombstone) return kNilNode;
  std::size_t i = static_cast<std::size_t>(mix(id)) & id_mask_;
  for (;;) {
    const OrderId k = id_keys_[i];
    if (k == id) return id_vals_[i];
    if (k == kEmptyKey) return kNilNode;
    i = (i + 1) & id_mask_;
  }
}

void OrderBook::id_insert(OrderId id, std::uint32_t slot) {
  // Tombstones count against the load factor, not just live entries. A book
  // that churns orders (add, fill, add, fill...) keeps a near-constant live
  // count while steadily filling the table with tombstones -- if only
  // id_count_ gated the rehash, the table would eventually contain no empty
  // slot at all and slot_for()'s probe would never terminate.
  if ((id_count_ + id_tombstones_ + 1) * 2 >= id_keys_.size()) id_grow();

  std::size_t i = static_cast<std::size_t>(mix(id)) & id_mask_;
  for (;;) {
    const OrderId k = id_keys_[i];
    if (k == kEmptyKey || k == kTombstone || k == id) {
      if (k == kTombstone) --id_tombstones_;
      if (k != id) ++id_count_;
      id_keys_[i] = id;
      id_vals_[i] = slot;
      return;
    }
    i = (i + 1) & id_mask_;
  }
}

void OrderBook::id_erase(OrderId id) {
  std::size_t i = static_cast<std::size_t>(mix(id)) & id_mask_;
  for (;;) {
    const OrderId k = id_keys_[i];
    if (k == id) {
      id_keys_[i] = kTombstone;
      id_vals_[i] = kNilNode;
      --id_count_;
      ++id_tombstones_;
      return;
    }
    if (k == kEmptyKey) return;
    i = (i + 1) & id_mask_;
  }
}

// Rehash into a table twice the *live* size, which also sweeps out tombstones.
void OrderBook::id_grow() {
  std::vector<OrderId> old_keys;
  std::vector<std::uint32_t> old_vals;
  old_keys.swap(id_keys_);
  old_vals.swap(id_vals_);

  std::size_t cap = 1024;
  while (cap < (id_count_ + 1) * 4) cap <<= 1;
  id_keys_.assign(cap, kEmptyKey);
  id_vals_.assign(cap, kNilNode);
  id_mask_ = cap - 1;
  id_count_ = 0;
  id_tombstones_ = 0;

  for (std::size_t j = 0; j < old_keys.size(); ++j) {
    const OrderId k = old_keys[j];
    if (k == kEmptyKey || k == kTombstone) continue;
    std::size_t i = static_cast<std::size_t>(mix(k)) & id_mask_;
    while (id_keys_[i] != kEmptyKey) i = (i + 1) & id_mask_;
    id_keys_[i] = k;
    id_vals_[i] = old_vals[j];
    ++id_count_;
  }
}

bool OrderBook::has_order(OrderId id) const { return slot_for(id) != kNilNode; }

// --- node slab --------------------------------------------------------------

std::uint32_t OrderBook::alloc_node() {
  if (free_head_ != kNilNode) {
    const std::uint32_t slot = free_head_;
    free_head_ = pool_[slot].next;
    pool_[slot] = OrderNode{};
    return slot;
  }
  pool_.push_back(OrderNode{});
  return static_cast<std::uint32_t>(pool_.size() - 1);
}

void OrderBook::free_node(std::uint32_t slot) {
  pool_[slot].live = false;
  pool_[slot].next = free_head_;
  free_head_ = slot;
}

void OrderBook::link_back(PriceLevel& level, std::uint32_t slot) {
  pool_[slot].prev = level.tail;
  pool_[slot].next = kNilNode;
  if (level.tail != kNilNode) {
    pool_[level.tail].next = slot;
  } else {
    level.head = slot;
  }
  level.tail = slot;
  ++level.order_count;
}

void OrderBook::unlink(PriceLevel& level, std::uint32_t slot) {
  const std::uint32_t prev = pool_[slot].prev;
  const std::uint32_t next = pool_[slot].next;
  if (prev != kNilNode) {
    pool_[prev].next = next;
  } else {
    level.head = next;
  }
  if (next != kNilNode) {
    pool_[next].prev = prev;
  } else {
    level.tail = prev;
  }
  --level.order_count;
}

// --- matching ---------------------------------------------------------------

Quantity OrderBook::match(OrderId aggressor_id, Side aggressor_side, Price limit,
                          Quantity qty, std::vector<Trade>* out) {
  const Side resting_side = opposite(aggressor_side);
  auto& levels = side_levels(resting_side);
  auto& map = side_bitmap(resting_side);

  Quantity filled = 0;
  while (qty > 0) {
    // Resting bids fill best (highest) first; resting asks best (lowest) first.
    const std::size_t idx =
        (resting_side == Side::Buy) ? map.find_highest() : map.find_lowest();
    if (idx == LevelBitmap::npos) break;

    const Price level_price = price_at(idx);
    if (limit != kNoPrice) {
      // A buy aggressor will not pay above its limit; a sell will not sell below.
      if (aggressor_side == Side::Buy && level_price > limit) break;
      if (aggressor_side == Side::Sell && level_price < limit) break;
    }

    PriceLevel& level = levels[idx];
    while (qty > 0 && level.head != kNilNode) {
      const std::uint32_t slot = level.head;  // FIFO: oldest resting order first
      OrderNode& node = pool_[slot];
      const Quantity traded = std::min(qty, node.quantity);

      node.quantity -= traded;
      level.total_quantity -= traded;
      qty -= traded;
      filled += traded;

      if (out) {
        out->push_back(Trade{aggressor_id, node.id, aggressor_side, level_price, traded});
      }

      if (node.quantity == 0) {
        const OrderId done_id = node.id;
        unlink(level, slot);
        id_erase(done_id);
        free_node(slot);
        --live_orders_;
      }
    }

    if (level.order_count == 0) {
      level.total_quantity = 0;  // defensive: should already be zero
      map.clear(idx);
    }
  }
  return filled;
}

Quantity OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty,
                              std::vector<Trade>* out) {
  if (qty <= 0 || id == kEmptyKey || id == kTombstone) return -1;
  if (!in_band(price)) return -1;
  if (slot_for(id) != kNilNode) return -1;  // duplicate live id

  const Quantity filled = match(id, side, price, qty, out);
  const Quantity remaining = qty - filled;
  if (remaining <= 0) return filled;

  const std::size_t idx = index_of(price);
  PriceLevel& level = side_levels(side)[idx];
  const std::uint32_t slot = alloc_node();
  pool_[slot].id = id;
  pool_[slot].price = price;
  pool_[slot].quantity = remaining;
  pool_[slot].side = side;
  pool_[slot].live = true;

  link_back(level, slot);
  level.total_quantity += remaining;
  side_bitmap(side).set(idx);
  id_insert(id, slot);
  ++live_orders_;

  return filled;
}

Quantity OrderBook::execute_market(OrderId id, Side side, Quantity qty,
                                   std::vector<Trade>* out) {
  if (qty <= 0) return 0;
  return match(id, side, kNoPrice, qty, out);
}

bool OrderBook::cancel(OrderId id) {
  const std::uint32_t slot = slot_for(id);
  if (slot == kNilNode) return false;

  OrderNode& node = pool_[slot];
  const std::size_t idx = index_of(node.price);
  PriceLevel& level = side_levels(node.side)[idx];

  level.total_quantity -= node.quantity;
  unlink(level, slot);
  if (level.order_count == 0) side_bitmap(node.side).clear(idx);

  id_erase(id);
  free_node(slot);
  --live_orders_;
  return true;
}

bool OrderBook::modify(OrderId id, Price new_price, Quantity new_qty,
                       std::vector<Trade>* out) {
  const std::uint32_t slot = slot_for(id);
  if (slot == kNilNode) return false;
  if (new_qty <= 0) return cancel(id);
  if (!in_band(new_price)) return false;  // reject before destroying the order

  OrderNode& node = pool_[slot];
  if (new_price == node.price && new_qty <= node.quantity) {
    // Size reduction in place: the order keeps its position in the FIFO queue.
    const Quantity delta = node.quantity - new_qty;
    node.quantity = new_qty;
    side_levels(node.side)[index_of(node.price)].total_quantity -= delta;
    return true;
  }

  // Reprice or size increase: cancel/replace, losing time priority. The
  // replacement may be marketable, so it goes through the normal add path.
  const Side side = node.side;
  cancel(id);
  add_limit(id, side, new_price, new_qty, out);
  return true;
}

void OrderBook::clear() {
  std::fill(bids_.begin(), bids_.end(), PriceLevel{});
  std::fill(asks_.begin(), asks_.end(), PriceLevel{});
  bid_map_.reset();
  ask_map_.reset();
  pool_.clear();
  free_head_ = kNilNode;
  live_orders_ = 0;
  std::fill(id_keys_.begin(), id_keys_.end(), kEmptyKey);
  std::fill(id_vals_.begin(), id_vals_.end(), kNilNode);
  id_count_ = 0;
  id_tombstones_ = 0;
}

// --- accessors --------------------------------------------------------------

Price OrderBook::best_bid() const {
  const std::size_t idx = bid_map_.find_highest();
  return idx == LevelBitmap::npos ? kNoPrice : price_at(idx);
}

Price OrderBook::best_ask() const {
  const std::size_t idx = ask_map_.find_lowest();
  return idx == LevelBitmap::npos ? kNoPrice : price_at(idx);
}

Quantity OrderBook::best_bid_quantity() const {
  const std::size_t idx = bid_map_.find_highest();
  return idx == LevelBitmap::npos ? 0 : bids_[idx].total_quantity;
}

Quantity OrderBook::best_ask_quantity() const {
  const std::size_t idx = ask_map_.find_lowest();
  return idx == LevelBitmap::npos ? 0 : asks_[idx].total_quantity;
}

bool OrderBook::spread(Price& out) const {
  const Price b = best_bid();
  const Price a = best_ask();
  if (b == kNoPrice || a == kNoPrice) return false;
  out = a - b;
  return true;
}

bool OrderBook::mid_price(double& out) const {
  const Price b = best_bid();
  const Price a = best_ask();
  if (b == kNoPrice || a == kNoPrice) return false;
  out = (price_to_double(b) + price_to_double(a)) * 0.5;
  return true;
}

Quantity OrderBook::quantity_at(Side side, Price price) const {
  if (!in_band(price)) return 0;
  return side_levels(side)[index_of(price)].total_quantity;
}

std::vector<OrderBook::LevelView> OrderBook::depth(Side side, std::size_t levels) const {
  std::vector<LevelView> out;
  if (levels == 0) return out;
  out.reserve(levels);

  const auto& lv = side_levels(side);
  const auto& map = side_bitmap(side);
  const bool descending = (side == Side::Buy);

  std::size_t idx = descending ? map.find_highest() : map.find_lowest();
  while (idx != LevelBitmap::npos && out.size() < levels) {
    out.push_back(LevelView{price_at(idx), lv[idx].total_quantity, lv[idx].order_count});
    idx = descending ? scan_below(map.l0_words(), idx)
                     : scan_above(map.l0_words(), map.bit_count(), idx);
  }
  return out;
}

}  // namespace hft
