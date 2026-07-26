#include "hft/latency.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

namespace hft {

// ------------------------------------------------------------------ TscClock

TscClock::TscClock() {
#if HFT_HAVE_TSC
  // Busy-wait rather than sleep: sleeping hands the core away and the wakeup
  // jitter (~1ms on Windows) would swamp a short calibration window.
  const Nanos t0 = now_ns();
  const std::uint64_t c0 = rdtsc();
  const Nanos target = t0 + 20'000'000;  // 20ms
  Nanos t1 = t0;
  while (t1 < target) t1 = now_ns();
  const std::uint64_t c1 = rdtsc();

  const double elapsed_ns = static_cast<double>(t1 - t0);
  const double elapsed_ticks = static_cast<double>(c1 - c0);
  if (elapsed_ticks > 0.0 && elapsed_ns > 0.0) {
    ns_per_tick_ = elapsed_ns / elapsed_ticks;
    ticks_per_ns_ = elapsed_ticks / elapsed_ns;
  }
#endif
}

// ----------------------------------------------------------- LatencyHistogram

std::size_t LatencyHistogram::bucket_index(Nanos value) noexcept {
  if (value < 0) value = 0;
  const std::uint64_t v = static_cast<std::uint64_t>(value);
  if (v < kSub) return static_cast<std::size_t>(v);

  const std::size_t msb = 63u - static_cast<std::size_t>(__builtin_clzll(v));
  const std::size_t shift = msb - kSubBits + 1;      // >= 1
  const std::size_t sub = static_cast<std::size_t>(v >> shift);  // in [kHalfSub, kSub)
  const std::size_t idx = kSub + (shift - 1) * kHalfSub + (sub - kHalfSub);
  return idx < kBuckets ? idx : kBuckets - 1;
}

Nanos LatencyHistogram::bucket_low(std::size_t index) noexcept {
  if (index < kSub) return static_cast<Nanos>(index);
  const std::size_t k = index - kSub;
  const std::size_t shift = k / kHalfSub + 1;
  const std::size_t sub = k % kHalfSub + kHalfSub;
  return static_cast<Nanos>(static_cast<std::uint64_t>(sub) << shift);
}

Nanos LatencyHistogram::bucket_high(std::size_t index) noexcept {
  if (index < kSub) return static_cast<Nanos>(index);
  const std::size_t k = index - kSub;
  const std::size_t shift = k / kHalfSub + 1;
  const std::size_t sub = k % kHalfSub + kHalfSub;
  return static_cast<Nanos>((static_cast<std::uint64_t>(sub + 1) << shift) - 1);
}

void LatencyHistogram::record(Nanos value) noexcept {
  if (value < 0) value = 0;
  ++counts_[bucket_index(value)];
  if (count_ == 0 || value < min_) min_ = value;
  if (count_ == 0 || value > max_) max_ = value;
  ++count_;
  sum_ += static_cast<std::uint64_t>(value);
}

void LatencyHistogram::merge(const LatencyHistogram& other) {
  if (other.count_ == 0) return;
  for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += other.counts_[i];
  if (count_ == 0) {
    min_ = other.min_;
    max_ = other.max_;
  } else {
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
  }
  count_ += other.count_;
  sum_ += other.sum_;
}

void LatencyHistogram::reset() {
  std::fill(counts_.begin(), counts_.end(), 0ULL);
  count_ = 0;
  sum_ = 0;
  min_ = 0;
  max_ = 0;
}

Nanos LatencyHistogram::percentile(double p) const {
  if (count_ == 0) return 0;
  if (p >= 100.0) return max_;
  if (p <= 0.0) return min_;

  // Rank of the first sample at or beyond the requested percentile.
  std::uint64_t target = static_cast<std::uint64_t>(
      (p / 100.0) * static_cast<double>(count_) + 0.5);
  if (target == 0) target = 1;
  if (target > count_) target = count_;

  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < kBuckets; ++i) {
    seen += counts_[i];
    if (seen >= target) {
      const Nanos hi = bucket_high(i);
      return hi > max_ ? max_ : hi;
    }
  }
  return max_;
}

std::vector<LatencyHistogram::Bucket> LatencyHistogram::buckets() const {
  std::vector<Bucket> out;
  for (std::size_t i = 0; i < kBuckets; ++i) {
    if (counts_[i] == 0) continue;
    out.push_back(Bucket{bucket_low(i), bucket_high(i), counts_[i]});
  }
  return out;
}

// ------------------------------------------------------------ LatencyRecorder

namespace {

struct NamedStage {
  const char* name;
  const LatencyHistogram* hist;
};

std::vector<NamedStage> stages_of(const LatencyRecorder& r) {
  return {
      {"ingest_to_book", &r.ingest_to_book},
      {"book_update", &r.book_update},
      {"signal_compute", &r.signal_compute},
      {"risk_check", &r.risk_check},
      {"order_round_trip", &r.order_round_trip},
      {"tick_to_order", &r.tick_to_order},
  };
}

}  // namespace

void LatencyRecorder::reset() {
  ingest_to_book.reset();
  book_update.reset();
  signal_compute.reset();
  risk_check.reset();
  order_round_trip.reset();
  tick_to_order.reset();
}

std::string LatencyRecorder::summary() const {
  std::ostringstream os;
  char line[256];

  std::snprintf(line, sizeof(line), "%-18s %10s %10s %10s %10s %10s %10s\n", "stage", "count",
                "p50(ns)", "p99(ns)", "p99.9(ns)", "max(ns)", "mean(ns)");
  os << line;
  os << std::string(84, '-') << "\n";

  for (const auto& s : stages_of(*this)) {
    if (s.hist->count() == 0) continue;
    std::snprintf(line, sizeof(line), "%-18s %10llu %10lld %10lld %10lld %10lld %10.1f\n", s.name,
                  static_cast<unsigned long long>(s.hist->count()),
                  static_cast<long long>(s.hist->percentile(50.0)),
                  static_cast<long long>(s.hist->percentile(99.0)),
                  static_cast<long long>(s.hist->percentile(99.9)),
                  static_cast<long long>(s.hist->max()), s.hist->mean());
    os << line;
  }
  return os.str();
}

bool LatencyRecorder::write_summary_csv(const std::string& path) const {
  std::ofstream f(path);
  if (!f) return false;
  f << "stage,count,percentile,latency_ns,mean_ns,min_ns,max_ns\n";
  for (const auto& s : stages_of(*this)) {
    if (s.hist->count() == 0) continue;
    for (double p : kReportedPercentiles) {
      f << s.name << "," << s.hist->count() << "," << p << "," << s.hist->percentile(p) << ","
        << s.hist->mean() << "," << s.hist->min() << "," << s.hist->max() << "\n";
    }
  }
  return static_cast<bool>(f);
}

bool LatencyRecorder::write_histogram_csv(const std::string& path) const {
  std::ofstream f(path);
  if (!f) return false;
  f << "stage,bucket_low_ns,bucket_high_ns,count\n";
  for (const auto& s : stages_of(*this)) {
    for (const auto& b : s.hist->buckets()) {
      f << s.name << "," << b.low << "," << b.high << "," << b.count << "\n";
    }
  }
  return static_cast<bool>(f);
}

}  // namespace hft
