#include "hft/feed.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "hft/latency.hpp"

namespace hft {

// ---------------------------------------------------------------- SyntheticFeed

SyntheticFeed::SyntheticFeed(Params params)
    : params_(params), state_(params.seed ? params.seed : 1), fair_(params.start_price) {
  live_ids_.reserve(4096);
}

// xorshift64*: fast, deterministic, and good enough for generating order flow.
// Not cryptographic and not trying to be -- the property that matters is that
// the same seed always produces the same stream.
std::uint64_t SyntheticFeed::rng() {
  state_ ^= state_ >> 12;
  state_ ^= state_ << 25;
  state_ ^= state_ >> 27;
  return state_ * 0x2545F4914F6CDD1DULL;
}

void SyntheticFeed::reset() {
  state_ = params_.seed ? params_.seed : 1;
  fair_ = params_.start_price;
  emitted_ = 0;
  next_id_ = 1;
  live_ids_.clear();
}

bool SyntheticFeed::next(Tick& out) {
  if (emitted_ >= params_.total_events) return false;
  ++emitted_;

  // Random-walk the fair value, clamped to the configured band.
  if (static_cast<int>(rng_range(100)) < params_.pct_move) {
    fair_ += (rng() & 1) ? 1 : -1;
    fair_ = std::max(params_.min_price + params_.tick_range + 1,
                     std::min<Price>(params_.max_price - params_.tick_range - 1, fair_));
  }

  const int roll = static_cast<int>(rng_range(100));
  const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
  const Quantity qty =
      params_.min_qty +
      static_cast<Quantity>(rng_range(static_cast<std::uint64_t>(
          params_.max_qty - params_.min_qty + 1)));

  out = Tick{};
  out.symbol = params_.symbol;
  out.side = side;
  out.quantity = qty;
  out.source_ts_ns = now_ns();
  // Sequence numbers start at 1, so that 0 keeps its meaning of "this feed does
  // not sequence" rather than colliding with a real first message.
  out.sequence = static_cast<std::uint64_t>(emitted_);

  if (roll < params_.pct_add || live_ids_.empty()) {
    // Passive add: buys below fair value, sells above, biased toward the touch
    // so the book actually has depth near the mid.
    const Price offset = 1 + static_cast<Price>(rng_range(
                                 static_cast<std::uint64_t>(params_.tick_range)));
    out.type = TickType::AddOrder;
    out.price = (side == Side::Buy) ? fair_ - offset : fair_ + offset;
    if (params_.tick_size > 1) {
      // Snap onto the instrument's grid, away from the mid so a buy stays
      // below fair value and a sell stays above it.
      const Price remainder = out.price % params_.tick_size;
      if (remainder != 0) {
        out.price = (side == Side::Buy) ? out.price - remainder
                                        : out.price + (params_.tick_size - remainder);
      }
      // Clamp back inside the band after snapping.
      if (out.price < params_.min_price) out.price += params_.tick_size;
      if (out.price > params_.max_price) out.price -= params_.tick_size;
    }
    out.order_id = next_id_++;
    if (live_ids_.size() < 200'000) live_ids_.push_back(out.order_id);
  } else if (roll < params_.pct_add + params_.pct_cancel) {
    // Cancel a random previously-added order. It may already have traded away,
    // in which case the book rejects the cancel -- which is realistic; feeds
    // routinely deliver cancels for orders you have already filled.
    const std::size_t i = static_cast<std::size_t>(rng_range(live_ids_.size()));
    out.type = TickType::CancelOrder;
    out.order_id = live_ids_[i];
    out.price = 0;
    live_ids_[i] = live_ids_.back();
    live_ids_.pop_back();
  } else {
    // Aggressive marketable order -- this is what generates trades.
    out.type = TickType::Trade;
    out.price = fair_;
    out.order_id = next_id_++;
    out.quantity = 1 + static_cast<Quantity>(rng_range(200));
  }

  out.ingest_ts_ns = now_ns();
  return true;
}

bool SyntheticFeed::write_replay_csv(const std::string& path, Params params, std::size_t count) {
  params.total_events = count;
  SyntheticFeed feed(params);
  std::ofstream f(path);
  if (!f) return false;
  f << "symbol,type,side,price,quantity,order_id,source_ts_ns,sequence\n";

  Tick t;
  std::size_t n = 0;
  while (n < count && feed.next(t)) {
    f << t.symbol << ',' << static_cast<int>(t.type) << ',' << static_cast<int>(t.side) << ','
      << t.price << ',' << t.quantity << ',' << t.order_id << ',' << t.source_ts_ns << ','
      << t.sequence << '\n';
    ++n;
  }
  return static_cast<bool>(f);
}

// -------------------------------------------------------------- MultiSymbolFeed

MultiSymbolFeed::MultiSymbolFeed(std::vector<SyntheticFeed::Params> per_symbol) {
  feeds_.reserve(per_symbol.size());
  for (const auto& params : per_symbol) {
    feeds_.push_back(std::make_unique<SyntheticFeed>(params));
  }
}

bool MultiSymbolFeed::next(Tick& out) {
  if (feeds_.empty()) return false;

  // Round-robin, skipping instruments that have run out. Deterministic, which
  // is the property that makes a multi-symbol run reproducible at all.
  for (std::size_t tried = 0; tried < feeds_.size(); ++tried) {
    const std::size_t index = (cursor_ + tried) % feeds_.size();
    if (feeds_[index]->next(out)) {
      cursor_ = (index + 1) % feeds_.size();
      // Overwrite the per-instrument sequence with the channel's own, matching
      // how a real multi-instrument feed numbers its messages.
      out.sequence = ++sequence_;
      return true;
    }
  }
  return false;
}

void MultiSymbolFeed::reset() {
  for (auto& feed : feeds_) feed->reset();
  cursor_ = 0;
  exhausted_ = 0;
  sequence_ = 0;
}

// --------------------------------------------------------------- CsvReplayFeed

CsvReplayFeed::CsvReplayFeed(const std::string& path) : path_(path) {
  std::ifstream f(path);
  if (!f) {
    error_ = "cannot open replay file: " + path;
    return;
  }

  std::string line;
  if (!std::getline(f, line)) {
    error_ = "replay file is empty: " + path;
    return;
  }
  // The first line is a header if it does not start with a digit.
  const bool has_header = line.empty() || !std::isdigit(static_cast<unsigned char>(line[0]));
  std::size_t lineno = 1;

  auto parse = [&](const std::string& row) -> bool {
    std::istringstream ss(row);
    std::string cell;
    long long vals[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 7; ++i) {
      if (!std::getline(ss, cell, ',')) return false;
      vals[i] = std::stoll(cell);
    }
    // The sequence column was added later. Captures without it still replay,
    // with sequence 0 -- which the feed monitor reads as "unsequenced" rather
    // than as a gap, so an old file does not look like a broken session.
    if (std::getline(ss, cell, ',') && !cell.empty()) vals[7] = std::stoll(cell);

    Tick t{};
    t.symbol = static_cast<SymbolId>(vals[0]);
    t.type = static_cast<TickType>(vals[1]);
    t.side = static_cast<Side>(vals[2]);
    t.price = static_cast<Price>(vals[3]);
    t.quantity = static_cast<Quantity>(vals[4]);
    t.order_id = static_cast<OrderId>(vals[5]);
    t.source_ts_ns = static_cast<Nanos>(vals[6]);
    t.sequence = static_cast<std::uint64_t>(vals[7]);
    ticks_.push_back(t);
    return true;
  };

  if (!has_header && !parse(line)) {
    error_ = "malformed row at line 1";
    return;
  }
  while (std::getline(f, line)) {
    ++lineno;
    if (line.empty()) continue;
    if (!parse(line)) {
      error_ = "malformed row at line " + std::to_string(lineno);
      return;
    }
  }
  ok_ = true;
}

bool CsvReplayFeed::next(Tick& out) {
  if (cursor_ >= ticks_.size()) return false;
  out = ticks_[cursor_++];
  // Stamp ingest time now: the recorded source timestamp belongs to the
  // original capture, so measuring against it would report hours of "latency".
  out.ingest_ts_ns = now_ns();
  return true;
}

void CsvReplayFeed::reset() { cursor_ = 0; }

}  // namespace hft
