// hft_engine -- standalone entrypoint.
//
// Wires feed -> ring buffer -> order book -> strategy -> paper venue and runs
// it to completion, then prints a report and (optionally) writes CSVs that the
// demo notebook charts.
//
// Everything runs offline: the default feed is a seeded deterministic
// generator, so the same command produces the same trades every time.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "hft/engine.hpp"
#include "hft/feed.hpp"
#include "hft/symbol_table.hpp"

using namespace hft;

namespace {

struct Options {
  std::size_t events = 2'000'000;
  std::uint64_t seed = 0x5EEDC0DEULL;
  std::size_t fast_window = 5;
  std::size_t slow_window = 20;
  Quantity order_quantity = 10;
  double slippage_bps = 1.0;
  double fee_bps = 0.5;
  bool threaded = true;
  bool cross_book = true;
  std::string replay_in;
  std::string replay_out;
  std::size_t replay_out_events = 200'000;
  std::string out_dir;
  std::size_t depth_levels = 15;
  std::string symbol = "SYNTH";
};

void usage(const char* argv0) {
  std::printf(
      "usage: %s [options]\n"
      "\n"
      "  --events N            synthetic events to generate      (default 2000000)\n"
      "  --seed N              PRNG seed for the synthetic feed   (default 1592356062)\n"
      "  --symbol NAME         symbol name to label output with   (default SYNTH)\n"
      "  --fast N              fast moving-average window         (default 5)\n"
      "  --slow N              slow moving-average window         (default 20)\n"
      "  --qty N               order quantity per signal          (default 10)\n"
      "  --slippage-bps X      fallback slippage model, bps       (default 1.0)\n"
      "  --fee-bps X           fee charged per fill, bps          (default 0.5)\n"
      "  --inline              run single-threaded (no ring hand-off)\n"
      "  --threaded            run the feed on its own thread     (default)\n"
      "  --no-cross-book       fill at reference price instead of crossing the book\n"
      "  --replay FILE         replay a captured CSV instead of generating\n"
      "  --write-replay FILE   write a deterministic replay CSV and exit\n"
      "  --replay-events N     rows to write with --write-replay  (default 200000)\n"
      "  --out-dir DIR         write latency/fill/book CSVs here\n"
      "  --depth N             book depth levels to snapshot      (default 15)\n"
      "  -h, --help            this message\n",
      argv0);
}

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      usage(argv[0]);
      std::exit(0);
    } else if (a == "--events") {
      o.events = std::strtoull(need("--events"), nullptr, 10);
    } else if (a == "--seed") {
      o.seed = std::strtoull(need("--seed"), nullptr, 10);
    } else if (a == "--symbol") {
      o.symbol = need("--symbol");
    } else if (a == "--fast") {
      o.fast_window = std::strtoull(need("--fast"), nullptr, 10);
    } else if (a == "--slow") {
      o.slow_window = std::strtoull(need("--slow"), nullptr, 10);
    } else if (a == "--qty") {
      o.order_quantity = std::strtoll(need("--qty"), nullptr, 10);
    } else if (a == "--slippage-bps") {
      o.slippage_bps = std::strtod(need("--slippage-bps"), nullptr);
    } else if (a == "--fee-bps") {
      o.fee_bps = std::strtod(need("--fee-bps"), nullptr);
    } else if (a == "--inline") {
      o.threaded = false;
    } else if (a == "--threaded") {
      o.threaded = true;
    } else if (a == "--no-cross-book") {
      o.cross_book = false;
    } else if (a == "--replay") {
      o.replay_in = need("--replay");
    } else if (a == "--write-replay") {
      o.replay_out = need("--write-replay");
    } else if (a == "--replay-events") {
      o.replay_out_events = std::strtoull(need("--replay-events"), nullptr, 10);
    } else if (a == "--out-dir") {
      o.out_dir = need("--out-dir");
    } else if (a == "--depth") {
      o.depth_levels = std::strtoull(need("--depth"), nullptr, 10);
    } else {
      std::fprintf(stderr, "error: unknown option '%s' (try --help)\n", a.c_str());
      return false;
    }
  }
  return true;
}

std::string join(const std::string& dir, const char* file) {
  if (dir.empty()) return file;
  const char last = dir[dir.size() - 1];
  return dir + ((last == '/' || last == '\\') ? "" : "/") + file;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse(argc, argv, opt)) return 2;

  SymbolTable symbols;
  const SymbolId sym = symbols.intern(opt.symbol);

  SyntheticFeed::Params fp;
  fp.symbol = sym;
  fp.seed = opt.seed;
  fp.total_events = opt.events;

  if (!opt.replay_out.empty()) {
    const bool ok =
        SyntheticFeed::write_replay_csv(opt.replay_out, fp, opt.replay_out_events);
    std::printf("%s %zu events -> %s\n", ok ? "wrote" : "FAILED to write",
                opt.replay_out_events, opt.replay_out.c_str());
    return ok ? 0 : 1;
  }

  std::unique_ptr<MarketDataSource> feed;
  if (!opt.replay_in.empty()) {
    auto replay = std::make_unique<CsvReplayFeed>(opt.replay_in);
    if (!replay->ok()) {
      std::fprintf(stderr, "error: %s\n", replay->error().c_str());
      return 1;
    }
    std::printf("replaying %zu captured events from %s\n", replay->size(),
                opt.replay_in.c_str());
    feed = std::move(replay);
  } else {
    feed = std::make_unique<SyntheticFeed>(fp);
  }

  EngineConfig cfg;
  cfg.fast_window = opt.fast_window;
  cfg.slow_window = opt.slow_window;
  cfg.order_quantity = opt.order_quantity;
  cfg.slippage_bps = opt.slippage_bps;
  cfg.fee_bps = opt.fee_bps;
  cfg.threaded = opt.threaded;
  cfg.cross_book = opt.cross_book;

  Engine engine(cfg);

  std::printf("\n=== hft_engine ===\n");
  std::printf("feed      : %s\n", feed->name());
  std::printf("mode      : %s, %s\n", cfg.threaded ? "threaded (SPSC ring hand-off)" : "inline",
              cfg.cross_book ? "crossing real book liquidity" : "reference-price fills");
  std::printf("strategy  : MovingAverageCrossover(fast=%zu, slow=%zu)\n", cfg.fast_window,
              cfg.slow_window);
  std::printf("venue     : PaperVenue(slippage=%.2fbps, fee=%.2fbps)\n", cfg.slippage_bps,
              cfg.fee_bps);
  std::printf("\nrunning...\n");

  const EngineStats stats = engine.run(*feed);

  std::printf("\n--- pipeline ---\n");
  std::printf("ticks processed     : %llu\n", (unsigned long long)stats.ticks);
  std::printf("  book adds         : %llu\n", (unsigned long long)stats.adds);
  std::printf("  cancels applied   : %llu\n", (unsigned long long)stats.cancels);
  std::printf("  cancels rejected  : %llu  (order had already traded away)\n",
              (unsigned long long)stats.cancel_rejected);
  std::printf("  aggressive orders : %llu\n", (unsigned long long)stats.aggressive_orders);
  std::printf("book trades matched : %llu\n", (unsigned long long)stats.book_trades);
  std::printf("strategy signals    : %llu\n", (unsigned long long)stats.signals);
  std::printf("orders sent         : %llu\n", (unsigned long long)stats.orders_sent);
  std::printf("ring buffer drops   : %llu\n", (unsigned long long)stats.dropped_ticks);
  std::printf("wall time           : %.3f s\n", (double)stats.wall_ns / 1e9);
  std::printf("throughput          : %.0f ticks/sec\n", stats.ticks_per_second());

  const OrderBook& book = engine.book();
  std::printf("\n--- final book ---\n");
  std::printf("resting orders      : %zu\n", book.live_order_count());
  const Price bb = book.best_bid();
  const Price ba = book.best_ask();
  if (bb != kNoPrice && ba != kNoPrice) {
    std::printf("best bid            : %.2f x %lld\n", price_to_double(bb),
                (long long)book.best_bid_quantity());
    std::printf("best ask            : %.2f x %lld\n", price_to_double(ba),
                (long long)book.best_ask_quantity());
    std::printf("spread              : %.2f\n", price_to_double(ba - bb));
  }
  std::printf("\ntop of book:\n");
  const auto bids = book.depth(Side::Buy, 5);
  const auto asks = book.depth(Side::Sell, 5);
  std::printf("      %-12s %-10s | %-12s %-10s\n", "bid px", "bid qty", "ask px", "ask qty");
  for (std::size_t i = 0; i < 5; ++i) {
    char lhs[48] = "                        ";
    char rhs[48] = "";
    if (i < bids.size()) {
      std::snprintf(lhs, sizeof(lhs), "%-12.2f %-10lld", price_to_double(bids[i].price),
                    (long long)bids[i].quantity);
    }
    if (i < asks.size()) {
      std::snprintf(rhs, sizeof(rhs), "%-12.2f %-10lld", price_to_double(asks[i].price),
                    (long long)asks[i].quantity);
    }
    std::printf("      %-23s | %s\n", lhs, rhs);
  }

  const PaperVenue& venue = engine.venue();
  std::printf("\n--- paper trading ---\n");
  std::printf("fills               : %llu\n", (unsigned long long)venue.fill_count());
  std::printf("position %-11s: %lld\n", opt.symbol.c_str(), (long long)venue.position(sym));
  std::printf("realized pnl        : %.2f\n", venue.realized_pnl());
  std::printf("fees paid           : %.2f\n", venue.fees_paid());
  if (bb != kNoPrice && ba != kNoPrice) {
    std::printf("equity (marked mid) : %.2f\n", venue.equity(sym, (bb + ba) / 2));
  }

  std::printf("\n--- latency (ns, one dev machine, not production hardware) ---\n");
  std::printf("%s", engine.latency().summary().c_str());

  if (!opt.out_dir.empty()) {
    const std::string lat = join(opt.out_dir, "latency_summary.csv");
    const std::string hist = join(opt.out_dir, "latency_histogram.csv");
    const std::string fills = join(opt.out_dir, "fills.csv");
    const std::string bookcsv = join(opt.out_dir, "book_snapshot.csv");
    const bool a = engine.latency().write_summary_csv(lat);
    const bool b = engine.latency().write_histogram_csv(hist);
    const bool c = venue.write_fills_csv(fills);
    const bool d = engine.write_book_snapshot_csv(bookcsv, opt.depth_levels);
    std::printf("\nwrote CSVs to %s: latency_summary=%d latency_histogram=%d fills=%d book=%d\n",
                opt.out_dir.c_str(), a, b, c, d);
    if (!(a && b && c && d)) {
      std::fprintf(stderr, "error: one or more CSV writes failed (does %s exist?)\n",
                   opt.out_dir.c_str());
      return 1;
    }
  }

  std::printf("\n");
  return 0;
}
