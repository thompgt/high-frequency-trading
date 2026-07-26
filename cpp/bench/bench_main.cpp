// hft_bench -- microbenchmarks + end-to-end pipeline latency.
//
// Method notes, because a benchmark you cannot interpret is worse than none:
//
//  * Throughput numbers are wall-clock over a large batch, so the per-op cost
//    of reading a clock is amortised to nothing.
//  * Per-op latency distributions are timed with rdtsc (2-3 cycles) rather
//    than steady_clock (a function call, tens of ns) -- otherwise the clock
//    would dominate operations that take ~50ns. The TSC is calibrated against
//    steady_clock at startup, and the measured clock overhead is printed so
//    you can see how much of the reported figure is instrumentation.
//  * Every workload is driven by a seeded PRNG, so runs are comparable.
//  * The compiler is prevented from deleting work via a sink barrier.
//
// These are single-machine numbers on a general-purpose OS with no core
// pinning, no isolated CPUs, and no tuned kernel. Treat them as a relative
// baseline for this code, not as production HFT latency figures.

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "hft/engine.hpp"
#include "hft/execution.hpp"
#include "hft/feed.hpp"
#include "hft/latency.hpp"
#include "hft/order_book.hpp"
#include "hft/ring_buffer.hpp"
#include "hft/strategy.hpp"

using namespace hft;

namespace {

// Stops the optimiser from deleting the work being measured.
template <typename T>
inline void sink(const T& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

struct Result {
  std::string name;
  std::string unit;
  double value = 0.0;
  std::uint64_t ops = 0;
  double seconds = 0.0;
};

std::vector<Result> results;

void report_rate(const std::string& name, std::uint64_t ops, Nanos elapsed_ns) {
  const double secs = static_cast<double>(elapsed_ns) / 1e9;
  const double rate = secs > 0 ? static_cast<double>(ops) / secs : 0.0;
  results.push_back(Result{name, "ops/sec", rate, ops, secs});
  std::printf("  %-46s %14.0f ops/sec   (%llu ops in %.3fs, %.1f ns/op)\n", name.c_str(), rate,
              (unsigned long long)ops, secs,
              ops ? static_cast<double>(elapsed_ns) / static_cast<double>(ops) : 0.0);
}

struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 1) {}
  std::uint64_t operator()() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1DULL;
  }
};

// ------------------------------------------------------------------- clocks

void bench_clocks(const TscClock& tsc) {
  std::printf("\n[clock overhead -- how much of a latency figure is the measurement itself]\n");

  constexpr int kN = 2'000'000;
  {
    const Nanos t0 = now_ns();
    for (int i = 0; i < kN; ++i) {
      const Nanos v = now_ns();
      sink(v);
    }
    const Nanos t1 = now_ns();
    std::printf("  %-46s %14.1f ns/call\n", "steady_clock now_ns()",
                static_cast<double>(t1 - t0) / kN);
  }
  {
    const Nanos t0 = now_ns();
    for (int i = 0; i < kN; ++i) {
      const std::uint64_t v = rdtsc();
      sink(v);
    }
    const Nanos t1 = now_ns();
    std::printf("  %-46s %14.1f ns/call\n", "rdtsc()", static_cast<double>(t1 - t0) / kN);
  }
  std::printf("  %-46s %14.4f ns/tick\n", "TSC calibration", tsc.ns_per_tick());
}

// --------------------------------------------------------------- order book

// Passive churn: add a resting order, cancel a resting order. This is the
// dominant message type on a real equity feed by a wide margin.
void bench_book_add_cancel() {
  constexpr int kWarm = 20000;
  constexpr int kOps = 2'000'000;
  OrderBook book(5000, 15000);
  Rng rng(0xB00C);
  std::vector<OrderId> live;
  live.reserve(kWarm + 16);
  OrderId next = 1;

  for (int i = 0; i < kWarm; ++i) {
    const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
    const Price px = (side == Side::Buy) ? 9900 - static_cast<Price>(rng() % 90)
                                         : 10100 + static_cast<Price>(rng() % 90);
    if (book.add_limit(next, side, px, 1 + static_cast<Quantity>(rng() % 100)) >= 0) {
      live.push_back(next);
    }
    ++next;
  }

  const Nanos t0 = now_ns();
  for (int i = 0; i < kOps; ++i) {
    const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
    const Price px = (side == Side::Buy) ? 9900 - static_cast<Price>(rng() % 90)
                                         : 10100 + static_cast<Price>(rng() % 90);
    book.add_limit(next, side, px, 1 + static_cast<Quantity>(rng() % 100));
    live.push_back(next++);
    const std::size_t victim = static_cast<std::size_t>(rng() % live.size());
    book.cancel(live[victim]);
    live[victim] = live.back();
    live.pop_back();
  }
  const Nanos t1 = now_ns();
  report_rate("order book: add + cancel pair", kOps, t1 - t0);
}

// Aggressive flow: every order crosses and consumes resting liquidity.
void bench_book_match_flow() {
  constexpr int kOps = 500'000;
  OrderBook book(5000, 15000);
  Rng rng(0xDEAD10CC);
  OrderId next = 1;

  // Refill helper keeps depth available so the aggressor always trades.
  auto refill = [&](Side side, int n) {
    for (int i = 0; i < n; ++i) {
      const Price px = (side == Side::Buy) ? 9990 - static_cast<Price>(rng() % 30)
                                           : 10010 + static_cast<Price>(rng() % 30);
      book.add_limit(next++, side, px, 50);
    }
  };
  refill(Side::Buy, 2000);
  refill(Side::Sell, 2000);

  std::vector<Trade> trades;
  trades.reserve(64);
  std::uint64_t matched = 0;

  const Nanos t0 = now_ns();
  for (int i = 0; i < kOps; ++i) {
    trades.clear();
    const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
    matched += static_cast<std::uint64_t>(book.execute_market(next++, side, 40, &trades));
    if ((i & 1023) == 0) {
      refill(Side::Buy, 600);
      refill(Side::Sell, 600);
    }
  }
  const Nanos t1 = now_ns();
  sink(matched);
  report_rate("order book: marketable order (matches depth)", kOps, t1 - t0);
}

void bench_book_top_of_book() {
  constexpr int kOps = 20'000'000;
  OrderBook book(5000, 15000);
  Rng rng(0x70B);
  for (int i = 0; i < 20000; ++i) {
    book.add_limit(static_cast<OrderId>(i + 1), (rng() & 1) ? Side::Buy : Side::Sell,
                   (rng() & 1) ? 9900 - static_cast<Price>(rng() % 90)
                               : 10100 + static_cast<Price>(rng() % 90),
                   10);
  }
  Price acc = 0;
  const Nanos t0 = now_ns();
  for (int i = 0; i < kOps; ++i) {
    acc += book.best_bid() + book.best_ask();
    sink(acc);
  }
  const Nanos t1 = now_ns();
  report_rate("order book: best bid + best ask lookup", kOps, t1 - t0);
}

// Per-operation latency distribution for a mixed, realistic message stream.
void bench_book_latency(const TscClock& tsc) {
  constexpr int kOps = 2'000'000;
  OrderBook book(5000, 15000);
  Rng rng(0x1A7E);
  std::vector<OrderId> live;
  live.reserve(200000);
  OrderId next = 1;
  LatencyHistogram hist;
  std::vector<Trade> trades;
  trades.reserve(64);

  for (int i = 0; i < 50000; ++i) {
    const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
    const Price px = (side == Side::Buy) ? 9990 - static_cast<Price>(rng() % 60)
                                         : 10010 + static_cast<Price>(rng() % 60);
    if (book.add_limit(next, side, px, 1 + static_cast<Quantity>(rng() % 100)) >= 0) {
      live.push_back(next);
    }
    ++next;
  }

  for (int i = 0; i < kOps; ++i) {
    const int roll = static_cast<int>(rng() % 100);
    const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
    trades.clear();

    const std::uint64_t c0 = rdtsc();
    if (roll < 60 || live.empty()) {
      const Price px = (side == Side::Buy) ? 9990 - static_cast<Price>(rng() % 60)
                                           : 10010 + static_cast<Price>(rng() % 60);
      book.add_limit(next, side, px, 1 + static_cast<Quantity>(rng() % 100), &trades);
      live.push_back(next++);
    } else if (roll < 92) {
      const std::size_t v = static_cast<std::size_t>(rng() % live.size());
      book.cancel(live[v]);
      live[v] = live.back();
      live.pop_back();
    } else {
      book.execute_market(next++, side, 1 + static_cast<Quantity>(rng() % 120), &trades);
    }
    const std::uint64_t c1 = rdtsc();
    hist.record(tsc.to_ns(c1 - c0));
  }

  std::printf("\n[order book: per-message latency, mixed add/cancel/trade flow]\n");
  std::printf("  count=%llu  p50=%lldns  p90=%lldns  p99=%lldns  p99.9=%lldns  max=%lldns\n",
              (unsigned long long)hist.count(), (long long)hist.percentile(50),
              (long long)hist.percentile(90), (long long)hist.percentile(99),
              (long long)hist.percentile(99.9), (long long)hist.max());
  results.push_back(Result{"order book message p50", "ns",
                           static_cast<double>(hist.percentile(50)), hist.count(), 0});
  results.push_back(Result{"order book message p99", "ns",
                           static_cast<double>(hist.percentile(99)), hist.count(), 0});
  results.push_back(Result{"order book message p99.9", "ns",
                           static_cast<double>(hist.percentile(99.9)), hist.count(), 0});
}

// -------------------------------------------------------------- ring buffer

void bench_ring_single_thread() {
  constexpr int kOps = 50'000'000;
  SpscRingBuffer<Tick> ring(1024);
  Tick in{}, out{};
  in.price = 1;
  const Nanos t0 = now_ns();
  for (int i = 0; i < kOps; ++i) {
    ring.try_push(in);
    ring.try_pop(out);
    sink(out.price);
  }
  const Nanos t1 = now_ns();
  report_rate("ring buffer: push+pop, same thread", kOps, t1 - t0);
}

void bench_ring_cross_thread() {
  constexpr int kOps = 20'000'000;
  SpscRingBuffer<Tick> ring(8192);
  const Nanos t0 = now_ns();

  std::thread producer([&] {
    Tick t{};
    for (int i = 0; i < kOps; ++i) {
      t.price = i;
      while (!ring.try_push(t)) std::this_thread::yield();
    }
  });

  Tick got{};
  std::int64_t consumed = 0;
  while (consumed < kOps) {
    if (ring.try_pop(got)) {
      ++consumed;
      sink(got.price);
    }
  }
  producer.join();
  const Nanos t1 = now_ns();
  report_rate("ring buffer: SPSC hand-off across 2 threads", kOps, t1 - t0);
}

// ------------------------------------------------------------------ strategy

void bench_strategy() {
  constexpr int kOps = 20'000'000;
  MovingAverageCrossover strat(5, 20);
  Rng rng(0x57A7);
  Tick t{};
  t.type = TickType::Trade;
  Signal sig{};
  const Nanos t0 = now_ns();
  for (int i = 0; i < kOps; ++i) {
    const Price px = 9900 + static_cast<Price>(rng() % 200);
    const bool fired = strat.on_tick(t, px, sig);
    sink(fired);
  }
  const Nanos t1 = now_ns();
  report_rate("strategy: MovingAverageCrossover on_tick", kOps, t1 - t0);
}

// ---------------------------------------------------------------- end to end

void bench_pipeline(bool threaded, const std::string& out_dir) {
  EngineConfig cfg;
  cfg.threaded = threaded;
  cfg.record_curve = false;  // keep the hot path allocation-free
  Engine engine(cfg);

  SyntheticFeed::Params p;
  p.total_events = 5'000'000;
  p.seed = 0xBEEF;
  SyntheticFeed feed(p);

  const EngineStats st = engine.run(feed);
  const std::string label =
      threaded ? "pipeline: end-to-end, feed on its own thread" : "pipeline: end-to-end, inline";
  report_rate(label, st.ticks, st.wall_ns);

  std::printf("\n[%s -- stage latencies, ns]\n", threaded ? "threaded" : "inline");
  std::printf("%s", engine.latency().summary().c_str());
  std::printf("  signals=%llu orders=%llu book trades=%llu realized pnl=%.2f\n",
              (unsigned long long)st.signals, (unsigned long long)st.orders_sent,
              (unsigned long long)st.book_trades, engine.venue().realized_pnl());

  const LatencyHistogram& e2e = engine.latency().tick_to_order;
  if (e2e.count() > 0) {
    results.push_back(
        Result{std::string("tick->order p50 (") + (threaded ? "threaded" : "inline") + ")", "ns",
               static_cast<double>(e2e.percentile(50)), e2e.count(), 0});
    results.push_back(
        Result{std::string("tick->order p99 (") + (threaded ? "threaded" : "inline") + ")", "ns",
               static_cast<double>(e2e.percentile(99)), e2e.count(), 0});
    results.push_back(
        Result{std::string("tick->order p99.9 (") + (threaded ? "threaded" : "inline") + ")", "ns",
               static_cast<double>(e2e.percentile(99.9)), e2e.count(), 0});
  }

  if (!out_dir.empty()) {
    const std::string tag = threaded ? "threaded" : "inline";
    engine.latency().write_summary_csv(out_dir + "/bench_latency_summary_" + tag + ".csv");
    engine.latency().write_histogram_csv(out_dir + "/bench_latency_histogram_" + tag + ".csv");
  }
}

void write_results_csv(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    std::fprintf(stderr, "warning: could not write %s\n", path.c_str());
    return;
  }
  std::fprintf(f, "benchmark,unit,value,ops,seconds\n");
  for (const auto& r : results) {
    std::fprintf(f, "\"%s\",%s,%.4f,%llu,%.6f\n", r.name.c_str(), r.unit.c_str(), r.value,
                 (unsigned long long)r.ops, r.seconds);
  }
  std::fclose(f);
  std::printf("\nwrote %s\n", path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  std::string out_dir;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) out_dir = argv[++i];
  }

  std::printf("\n================ hft C++ engine benchmarks ================\n");
  std::printf("Single dev machine, general-purpose OS, no core pinning.\n");
  std::printf("Relative baseline for this code -- NOT production HFT latency.\n");

  TscClock tsc;
  bench_clocks(tsc);

  std::printf("\n[throughput]\n");
  bench_book_add_cancel();
  bench_book_match_flow();
  bench_book_top_of_book();
  bench_ring_single_thread();
  bench_ring_cross_thread();
  bench_strategy();

  bench_book_latency(tsc);

  std::printf("\n[end-to-end pipeline]\n");
  bench_pipeline(false, out_dir);
  bench_pipeline(true, out_dir);

  if (!out_dir.empty()) write_results_csv(out_dir + "/bench_results.csv");
  std::printf("\n===========================================================\n\n");
  return 0;
}
