// Instrument definitions and the registry that owns them.
//
// A single hardcoded price band and an implied "everything is one symbol" is
// fine for a demo and wrong for anything else. Real instruments differ in the
// ways that matter to every layer below:
//
//   * **Price band.** The book is a flat array over a fixed band, so the band
//     is per instrument or it is uselessly wide. A $3 stock and a $400 stock
//     cannot share one.
//   * **Tick size.** Venues reject a price that is not a multiple of the tick,
//     and a strategy that emits one is broken. Catching it here costs a modulo;
//     catching it at the venue costs a rejected order and a puzzled operator.
//   * **Lot size.** Same argument for quantity.
//   * **Position limits.** A limit that is right for a liquid future is
//     recklessly large for a thin name. One global number cannot be correct
//     for both, so an instrument may tighten it.
//
// The registry is built once at startup and read-only thereafter, so lookups on
// the tick path are a bounds check and an index -- no hashing, no locking.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "hft/types.hpp"

namespace hft {

struct Instrument {
  SymbolId id = 0;
  std::string symbol;

  // Inclusive price band for this instrument's book, in ticks.
  Price min_price = 5000;
  Price max_price = 15000;

  // Prices must be a multiple of this. 1 means every tick is valid.
  Price tick_size = 1;
  // Quantities must be a multiple of this.
  Quantity lot_size = 1;

  // Per-instrument position ceiling. Zero means "use the global limit"; a
  // non-zero value may only ever tighten it, never loosen it (enforced by the
  // risk manager, so a config mistake cannot widen a limit by accident).
  std::int64_t max_position = 0;

  bool price_is_valid(Price p) const {
    return p >= min_price && p <= max_price && (tick_size <= 1 || p % tick_size == 0);
  }
  bool quantity_is_valid(Quantity q) const {
    return q > 0 && (lot_size <= 1 || q % lot_size == 0);
  }
  // Rounds toward the passive side: a buy rounds down, a sell rounds up, so
  // rounding never crosses further into the market than intended.
  Price round_price(Price p, Side side) const;
};

class InstrumentRegistry {
 public:
  // Adds an instrument, assigning it the next id. Returns false with a reason
  // if the definition is unusable or the name is already taken -- a duplicate
  // symbol silently mapping to two ids would be a very quiet disaster.
  bool add(Instrument instrument, std::string& error);

  const Instrument* find(SymbolId id) const {
    return id < instruments_.size() ? &instruments_[id] : nullptr;
  }
  const Instrument* find(const std::string& symbol) const;

  bool contains(SymbolId id) const { return id < instruments_.size(); }
  std::size_t size() const { return instruments_.size(); }
  bool empty() const { return instruments_.empty(); }
  const std::vector<Instrument>& all() const { return instruments_; }

  // Widest band across every instrument. Used for sizing shared structures.
  Price min_price() const;
  Price max_price() const;

  std::string summary() const;
  void clear();

 private:
  std::vector<Instrument> instruments_;  // indexed by SymbolId
  std::unordered_map<std::string, SymbolId> by_name_;
};

// Parses "SYMBOL:min:max[:tick[:lot[:max_position]]]", the form used in config
// files and on the command line. Prices are in ticks, as everywhere else.
bool parse_instrument(const std::string& spec, Instrument& out, std::string& error);

}  // namespace hft
