// Core value types shared across the engine.
//
// Design notes:
//  * Prices are integers ("ticks"), never doubles. Floating-point prices make
//    exact price-level bucketing and equality comparison unreliable, which is
//    fatal for an order book. One tick == one cent by default (see kTickScale).
//  * Every struct here is trivially copyable and fits comfortably in a cache
//    line so it can be memcpy'd through the SPSC ring buffer without any
//    allocation on the hot path.
#pragma once

#include <cstdint>
#include <string>

namespace hft {

using Price = std::int64_t;     // integer price ticks (cents by default)
using Quantity = std::int64_t;  // share/contract count
using OrderId = std::uint64_t;
using SymbolId = std::uint32_t;
using Nanos = std::int64_t;

// Ticks per unit of currency. Price 12345 == $123.45.
inline constexpr std::int64_t kTickScale = 100;

inline constexpr Price price_from_double(double px) {
  return static_cast<Price>(px * kTickScale + (px >= 0 ? 0.5 : -0.5));
}

inline constexpr double price_to_double(Price px) {
  return static_cast<double>(px) / static_cast<double>(kTickScale);
}

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

inline constexpr Side opposite(Side s) {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

inline constexpr const char* to_string(Side s) {
  return s == Side::Buy ? "BUY" : "SELL";
}

// A market data event. `type` distinguishes a plain trade print from a
// book-affecting event, so one feed can drive both the strategy and the book.
enum class TickType : std::uint8_t {
  Trade = 0,   // a print: price + size crossed
  Quote = 1,   // top-of-book update
  AddOrder = 2,
  CancelOrder = 3,
};

struct Tick {
  SymbolId symbol = 0;
  TickType type = TickType::Trade;
  Side side = Side::Buy;
  Price price = 0;
  Quantity quantity = 0;
  OrderId order_id = 0;
  Nanos source_ts_ns = 0;  // timestamp stamped by the feed/exchange
  Nanos ingest_ts_ns = 0;  // timestamp stamped when we received it
};

// Emitted by a Strategy when it wants to trade.
struct Signal {
  SymbolId symbol = 0;
  Side side = Side::Buy;
  Price price = 0;             // reference price at signal time
  Nanos tick_ingest_ts_ns = 0; // carried through for end-to-end latency
};

enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };

struct Order {
  OrderId id = 0;
  SymbolId symbol = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  Price price = 0;  // ignored for Market
  Quantity quantity = 0;
  Nanos created_ts_ns = 0;
};

struct Fill {
  OrderId order_id = 0;
  SymbolId symbol = 0;
  Side side = Side::Buy;
  Price price = 0;
  Quantity quantity = 0;
  double fee = 0.0;  // currency units, not ticks
  Nanos filled_ts_ns = 0;
};

// A match produced inside the order book: an aggressor consumed resting
// liquidity. `price` is always the resting order's price (price improvement
// accrues to the aggressor, as on a real exchange).
struct Trade {
  OrderId aggressor_id = 0;
  OrderId resting_id = 0;
  Side aggressor_side = Side::Buy;
  Price price = 0;
  Quantity quantity = 0;
};

}  // namespace hft
