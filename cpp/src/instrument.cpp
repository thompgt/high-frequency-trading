#include "hft/instrument.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace hft {
namespace {

// The book indexes its levels by (price - min_price), so the band size is a
// hard structural limit rather than a policy one: the level bitmap is three
// tiers of 64 bits.
constexpr Price kMaxBandTicks = 64 * 64 * 64;

bool parse_i64_strict(const std::string& text, long long& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const long long parsed = std::strtoll(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0') return false;
  out = parsed;
  return true;
}

}  // namespace

Price Instrument::round_price(Price p, Side side) const {
  if (tick_size <= 1) return p;
  const Price remainder = p % tick_size;
  if (remainder == 0) return p;
  // Buys round down and sells round up, so rounding always moves the order
  // away from the market rather than deeper into it.
  return side == Side::Buy ? p - remainder : p + (tick_size - remainder);
}

bool InstrumentRegistry::add(Instrument instrument, std::string& error) {
  if (instrument.symbol.empty()) {
    error = "instrument symbol must not be empty";
    return false;
  }
  if (by_name_.count(instrument.symbol) != 0) {
    error = "duplicate instrument '" + instrument.symbol + "'";
    return false;
  }
  if (instrument.min_price <= 0 || instrument.max_price <= instrument.min_price) {
    error = "instrument '" + instrument.symbol +
            "' needs 0 < min_price < max_price, got " + std::to_string(instrument.min_price) +
            ".." + std::to_string(instrument.max_price);
    return false;
  }
  if (instrument.max_price - instrument.min_price + 1 > kMaxBandTicks) {
    error = "instrument '" + instrument.symbol + "' price band is " +
            std::to_string(instrument.max_price - instrument.min_price + 1) +
            " ticks, the book supports at most " + std::to_string(kMaxBandTicks);
    return false;
  }
  if (instrument.tick_size <= 0) {
    error = "instrument '" + instrument.symbol + "' tick_size must be positive";
    return false;
  }
  if (instrument.lot_size <= 0) {
    error = "instrument '" + instrument.symbol + "' lot_size must be positive";
    return false;
  }
  if (instrument.max_position < 0) {
    error = "instrument '" + instrument.symbol + "' max_position must not be negative";
    return false;
  }
  // A band that contains no valid price is a configuration mistake that would
  // otherwise show up as "every order is rejected" at run time.
  if (instrument.tick_size > 1) {
    const Price first =
        instrument.min_price % instrument.tick_size == 0
            ? instrument.min_price
            : instrument.min_price + (instrument.tick_size - instrument.min_price %
                                                                 instrument.tick_size);
    if (first > instrument.max_price) {
      error = "instrument '" + instrument.symbol + "' has no price on a " +
              std::to_string(instrument.tick_size) + "-tick grid within its band";
      return false;
    }
  }

  instrument.id = static_cast<SymbolId>(instruments_.size());
  by_name_.emplace(instrument.symbol, instrument.id);
  instruments_.push_back(std::move(instrument));
  return true;
}

const Instrument* InstrumentRegistry::find(const std::string& symbol) const {
  const auto it = by_name_.find(symbol);
  return it == by_name_.end() ? nullptr : &instruments_[it->second];
}

Price InstrumentRegistry::min_price() const {
  Price lo = 0;
  bool first = true;
  for (const auto& i : instruments_) {
    if (first || i.min_price < lo) lo = i.min_price;
    first = false;
  }
  return lo;
}

Price InstrumentRegistry::max_price() const {
  Price hi = 0;
  for (const auto& i : instruments_) hi = std::max(hi, i.max_price);
  return hi;
}

void InstrumentRegistry::clear() {
  instruments_.clear();
  by_name_.clear();
}

std::string InstrumentRegistry::summary() const {
  std::ostringstream os;
  os << "instruments (" << instruments_.size() << "):\n";
  for (const auto& i : instruments_) {
    os << "  [" << i.id << "] " << i.symbol << "  band " << price_to_double(i.min_price) << ".."
       << price_to_double(i.max_price) << "  tick " << i.tick_size << "  lot " << i.lot_size;
    if (i.max_position > 0) os << "  max_position " << i.max_position;
    os << "\n";
  }
  return os.str();
}

bool parse_instrument(const std::string& spec, Instrument& out, std::string& error) {
  std::vector<std::string> parts;
  std::istringstream ss(spec);
  std::string cell;
  while (std::getline(ss, cell, ':')) parts.push_back(cell);

  if (parts.size() < 3 || parts.size() > 6) {
    error = "expected SYMBOL:min:max[:tick[:lot[:max_position]]], got '" + spec + "'";
    return false;
  }

  out = Instrument{};
  out.symbol = parts[0];
  if (out.symbol.empty()) {
    error = "instrument symbol must not be empty in '" + spec + "'";
    return false;
  }

  // Every numeric field is parsed strictly: "10abc" is an error, not 10. A
  // typo'd price band that silently becomes something else is precisely the
  // kind of thing this whole config layer exists to catch.
  struct Field {
    const char* name;
    long long* target;
  };
  long long min_price = 0, max_price = 0, tick = 1, lot = 1, max_position = 0;
  const Field fields[] = {{"min_price", &min_price},
                          {"max_price", &max_price},
                          {"tick_size", &tick},
                          {"lot_size", &lot},
                          {"max_position", &max_position}};
  for (std::size_t i = 1; i < parts.size(); ++i) {
    if (!parse_i64_strict(parts[i], *fields[i - 1].target)) {
      error = std::string(fields[i - 1].name) + " must be an integer in '" + spec + "', got '" +
              parts[i] + "'";
      return false;
    }
  }

  out.min_price = static_cast<Price>(min_price);
  out.max_price = static_cast<Price>(max_price);
  out.tick_size = static_cast<Price>(tick);
  out.lot_size = static_cast<Quantity>(lot);
  out.max_position = static_cast<std::int64_t>(max_position);
  return true;
}

}  // namespace hft
