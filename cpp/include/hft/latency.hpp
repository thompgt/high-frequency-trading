// Nanosecond latency instrumentation.
//
// Port of hft/metrics/timing.py, but the Python version keeps every raw sample
// in a list and sorts on demand -- fine for thousands of samples, useless for
// the tens of millions a benchmark produces. This uses a fixed-size
// HdrHistogram-style bucketed histogram instead: constant memory, no
// allocation on the record path, and percentiles computable at any time.
//
// Bucketing: values below 32ns get their own exact bucket. Above that, each
// power-of-two octave is split into 16 linear sub-buckets, so the reported
// value is within ~3% of the true value at any magnitude. That is far more
// resolution than the measurement noise floor on a general-purpose OS.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "hft/types.hpp"

namespace hft {

// Monotonic wall clock. steady_clock is the portable, correct choice: it never
// jumps backwards and on Windows/MSYS it is backed by QueryPerformanceCounter.
inline Nanos now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Raw timestamp counter. Cheaper than now_ns() (a couple of cycles vs a
// function call into the clock), which matters when you are timing operations
// that themselves take tens of nanoseconds -- otherwise the clock dominates
// what you are trying to measure. Only meaningful on x86-64 with an invariant
// TSC; the ticks must be converted with TscClock below, never assumed to be ns.
#if defined(__x86_64__) || defined(_M_X64)
#define HFT_HAVE_TSC 1
#include <x86intrin.h>
inline std::uint64_t rdtsc() noexcept { return __rdtsc(); }
#else
#define HFT_HAVE_TSC 0
inline std::uint64_t rdtsc() noexcept { return static_cast<std::uint64_t>(now_ns()); }
#endif

// Calibrates TSC ticks against steady_clock so raw counter deltas can be
// reported in nanoseconds.
class TscClock {
 public:
  TscClock();
  double ns_per_tick() const { return ns_per_tick_; }
  double ticks_per_ns() const { return ticks_per_ns_; }
  Nanos to_ns(std::uint64_t ticks) const {
    return static_cast<Nanos>(static_cast<double>(ticks) * ns_per_tick_);
  }
  bool usable() const { return HFT_HAVE_TSC && ns_per_tick_ > 0.0; }

 private:
  double ns_per_tick_ = 0.0;
  double ticks_per_ns_ = 0.0;
};

class LatencyHistogram {
 public:
  static constexpr std::size_t kSubBits = 5;
  static constexpr std::size_t kSub = 1u << kSubBits;   // 32
  static constexpr std::size_t kHalfSub = kSub / 2;     // 16 sub-buckets per octave
  static constexpr std::size_t kBuckets = 1024;

  LatencyHistogram() : counts_(kBuckets, 0) {}

  void record(Nanos value) noexcept;
  void merge(const LatencyHistogram& other);
  void reset();

  std::uint64_t count() const { return count_; }
  Nanos min() const { return count_ ? min_ : 0; }
  Nanos max() const { return count_ ? max_ : 0; }
  double mean() const { return count_ ? static_cast<double>(sum_) / count_ : 0.0; }

  // Percentile in [0, 100]. Returns the upper edge of the containing bucket,
  // i.e. "at least this fast for p% of samples".
  Nanos percentile(double p) const;

  struct Bucket {
    Nanos low;
    Nanos high;
    std::uint64_t count;
  };
  // Non-empty buckets only -- this is what gets written out for plotting.
  std::vector<Bucket> buckets() const;

  static std::size_t bucket_index(Nanos value) noexcept;
  static Nanos bucket_low(std::size_t index) noexcept;
  static Nanos bucket_high(std::size_t index) noexcept;

 private:
  std::vector<std::uint64_t> counts_;
  std::uint64_t count_ = 0;
  std::uint64_t sum_ = 0;
  Nanos min_ = 0;
  Nanos max_ = 0;
};

// Named collection of the pipeline's stage latencies. Mirrors the stages the
// Python LatencyRecorder tracked, plus the order-book stage that Python had no
// equivalent of.
class LatencyRecorder {
 public:
  LatencyHistogram ingest_to_book;   // feed timestamp -> book updated
  LatencyHistogram book_update;      // time inside OrderBook::apply
  LatencyHistogram signal_compute;   // time inside Strategy::on_tick
  LatencyHistogram risk_check;       // time inside RiskManager::check
  LatencyHistogram order_round_trip; // Strategy signal -> venue fill returned
  LatencyHistogram tick_to_order;    // end-to-end: feed timestamp -> fill

  void reset();
  std::string summary() const;
  // Writes one row per (stage, percentile) plus a second file of raw buckets.
  bool write_summary_csv(const std::string& path) const;
  bool write_histogram_csv(const std::string& path) const;
};

// Percentiles reported everywhere in this project, so the README table and the
// notebook cannot drift apart.
inline constexpr std::array<double, 6> kReportedPercentiles = {50.0, 90.0, 99.0, 99.9, 99.99, 100.0};

}  // namespace hft
