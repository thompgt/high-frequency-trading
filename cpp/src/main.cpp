// hft_engine -- standalone entrypoint.
//
// Lifecycle:
//   1. Parse config (file first, then CLI overrides, then cross-validate).
//   2. Start the async logger, log the effective configuration.
//   3. Install SIGINT/SIGTERM handlers that request a cooperative stop.
//   4. Run feed -> ring buffer -> order book -> strategy -> risk -> venue.
//   5. On exit: flatten inventory, write metrics/CSVs, flush logs, join.
//
// Every failure path returns a distinct non-zero exit code and says why. There
// is no silent catch-and-continue anywhere in this file.
//
// Everything runs offline: the default feed is a seeded deterministic
// generator, so the same command produces the same trades every time.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hft/config.hpp"
#include "hft/engine.hpp"
#include "hft/feed.hpp"
#include "hft/log.hpp"
#include "hft/metrics.hpp"
#include "hft/symbol_table.hpp"

using namespace hft;

namespace {

// Exit codes, so a supervisor can tell a config mistake from a runtime fault.
constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitConfig = 3;
constexpr int kExitIo = 4;
constexpr int kExitRuntime = 5;

// The only thing a signal handler may safely touch.
volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void handle_signal(int) { g_stop_requested = 1; }

void usage(const char* argv0) {
  std::printf(
      "usage: %s [options]\n"
      "\n"
      "  --config FILE         load settings from a config file (see cpp/config/)\n"
      "  --set KEY=VALUE       override one setting; repeatable, applied after --config\n"
      "  --list-settings       print every recognised setting name and exit\n"
      "  --print-config        print the effective configuration and exit\n"
      "\n"
      "  --events N            synthetic events to generate\n"
      "  --seed N              PRNG seed for the synthetic feed\n"
      "  --symbol NAME         symbol name to label output with\n"
      "  --replay FILE         replay a captured CSV instead of generating\n"
      "  --write-replay FILE   write a deterministic replay CSV and exit\n"
      "  --replay-events N     rows to write with --write-replay (default 200000)\n"
      "  --out-dir DIR         write metrics.json and CSVs here\n"
      "  --inline              run single-threaded (no ring hand-off)\n"
      "  --threaded            run the feed on its own thread\n"
      "  --no-risk             disable pre-trade risk limits (NOT recommended)\n"
      "  --log-level LEVEL     trace|debug|info|warn|error|off\n"
      "  -h, --help            this message\n"
      "\n"
      "Any setting can also be given as --set key=value; run --list-settings\n"
      "for the full list. Config file and CLI share one parser, so validation\n"
      "is identical for both.\n",
      argv0);
}

// Returns false if the CLI was malformed; `exit_now` requests a clean early
// exit (e.g. --help).
bool parse_cli(int argc, char** argv, AppConfig& cfg, std::string& config_path,
               std::string& replay_out, std::size_t& replay_out_events, bool& exit_now,
               int& exit_code) {
  std::vector<std::pair<std::string, std::string>> overrides;
  bool print_config = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", what);
        exit_now = true;
        exit_code = kExitUsage;
        return std::string();
      }
      return argv[++i];
    };

    if (a == "-h" || a == "--help") {
      usage(argv[0]);
      exit_now = true;
      exit_code = kExitOk;
      return true;
    } else if (a == "--list-settings") {
      for (const auto& k : config_keys()) std::printf("%s\n", k.c_str());
      exit_now = true;
      exit_code = kExitOk;
      return true;
    } else if (a == "--print-config") {
      print_config = true;
    } else if (a == "--config") {
      config_path = need("--config");
    } else if (a == "--set") {
      const std::string kv = need("--set");
      if (exit_now) return true;
      const std::size_t eq = kv.find('=');
      if (eq == std::string::npos) {
        std::fprintf(stderr, "error: --set expects KEY=VALUE, got '%s'\n", kv.c_str());
        exit_code = kExitUsage;
        return false;
      }
      overrides.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    } else if (a == "--write-replay") {
      replay_out = need("--write-replay");
    } else if (a == "--replay-events") {
      replay_out_events = std::strtoull(need("--replay-events").c_str(), nullptr, 10);
    } else if (a == "--inline") {
      overrides.emplace_back("threaded", "false");
    } else if (a == "--threaded") {
      overrides.emplace_back("threaded", "true");
    } else if (a == "--no-risk") {
      overrides.emplace_back("risk_enabled", "false");
    } else if (a == "--events") {
      overrides.emplace_back("events", need("--events"));
    } else if (a == "--seed") {
      overrides.emplace_back("seed", need("--seed"));
    } else if (a == "--symbol") {
      overrides.emplace_back("symbol", need("--symbol"));
    } else if (a == "--replay") {
      overrides.emplace_back("replay_path", need("--replay"));
    } else if (a == "--out-dir") {
      overrides.emplace_back("out_dir", need("--out-dir"));
    } else if (a == "--log-level") {
      overrides.emplace_back("log_level", need("--log-level"));
    } else {
      std::fprintf(stderr, "error: unknown option '%s' (try --help)\n", a.c_str());
      exit_code = kExitUsage;
      return false;
    }
    if (exit_now) return true;
  }

  // Config file first so CLI overrides win, which is the order people expect.
  if (!config_path.empty()) {
    std::vector<ConfigError> errors;
    if (!load_config_file(config_path, cfg, errors)) {
      std::fprintf(stderr, "error: %zu problem(s) in %s:\n", errors.size(), config_path.c_str());
      for (const auto& e : errors) {
        if (e.line > 0) {
          std::fprintf(stderr, "  line %zu: %s\n", e.line, e.message.c_str());
        } else {
          std::fprintf(stderr, "  %s\n", e.message.c_str());
        }
      }
      exit_code = kExitConfig;
      return false;
    }
  }

  for (const auto& kv : overrides) {
    std::string error;
    if (!apply_config_setting(kv.first, kv.second, cfg, error)) {
      std::fprintf(stderr, "error: --set %s: %s\n", kv.first.c_str(), error.c_str());
      exit_code = kExitConfig;
      return false;
    }
  }

  std::vector<ConfigError> errors;
  if (!validate_config(cfg, errors)) {
    std::fprintf(stderr, "error: invalid configuration:\n");
    for (const auto& e : errors) std::fprintf(stderr, "  %s\n", e.message.c_str());
    exit_code = kExitConfig;
    return false;
  }

  if (print_config) {
    std::printf("%s", describe_config(cfg).c_str());
    exit_now = true;
    exit_code = kExitOk;
  }
  return true;
}

std::string join(const std::string& dir, const char* file) {
  if (dir.empty()) return file;
  const char last = dir[dir.size() - 1];
  return dir + ((last == '/' || last == '\\') ? "" : "/") + file;
}

int run(int argc, char** argv) {
  AppConfig cfg;
  std::string config_path;
  std::string replay_out;
  std::size_t replay_out_events = 200'000;
  bool exit_now = false;
  int exit_code = kExitOk;

  if (!parse_cli(argc, argv, cfg, config_path, replay_out, replay_out_events, exit_now,
                 exit_code)) {
    return exit_code;
  }
  if (exit_now) return exit_code;

  SymbolTable symbols;
  const SymbolId sym = symbols.intern(cfg.symbol);

  SyntheticFeed::Params fp;
  fp.symbol = sym;
  fp.seed = cfg.seed;
  fp.total_events = cfg.events;
  fp.min_price = cfg.engine.min_price;
  fp.max_price = cfg.engine.max_price;
  fp.start_price = (cfg.engine.min_price + cfg.engine.max_price) / 2;

  // --write-replay is a standalone utility mode: produce a capture file, exit.
  if (!replay_out.empty()) {
    if (!SyntheticFeed::write_replay_csv(replay_out, fp, replay_out_events)) {
      std::fprintf(stderr, "error: could not write replay file '%s'\n", replay_out.c_str());
      return kExitIo;
    }
    std::printf("wrote %zu events -> %s\n", replay_out_events, replay_out.c_str());
    return kExitOk;
  }

  // --- logging -------------------------------------------------------------
  Logger& log = default_logger();
  log.set_level(cfg.log_level);
  log.start();

  std::printf("\n=== hft_engine ===\n");
  if (!config_path.empty()) std::printf("config file: %s\n", config_path.c_str());
  std::printf("\neffective configuration:\n%s\n", describe_config(cfg).c_str());

  // --- feed ----------------------------------------------------------------
  std::unique_ptr<MarketDataSource> feed;
  if (!cfg.replay_path.empty()) {
    auto replay = std::make_unique<CsvReplayFeed>(cfg.replay_path);
    if (!replay->ok()) {
      std::fprintf(stderr, "error: %s\n", replay->error().c_str());
      log.stop();
      return kExitIo;
    }
    std::printf("replaying %zu captured events from %s\n", replay->size(),
                cfg.replay_path.c_str());
    feed = std::move(replay);
  } else {
    feed = std::make_unique<SyntheticFeed>(fp);
  }

  // --- engine --------------------------------------------------------------
  std::unique_ptr<Engine> engine;
  try {
    engine = std::make_unique<Engine>(cfg.engine);
  } catch (const std::exception& e) {
    // Constructing the book validates the price band; a bad band is a config
    // error, not a crash.
    std::fprintf(stderr, "error: could not construct engine: %s\n", e.what());
    log.stop();
    return kExitConfig;
  }

  // --- signals -------------------------------------------------------------
  // The handler only sets a flag. A watcher thread turns that flag into a
  // cooperative stop, because calling into the engine from a signal context
  // is not safe.
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  std::atomic<bool> watcher_done{false};
  std::thread watcher([&] {
    while (!watcher_done.load(std::memory_order_acquire)) {
      if (g_stop_requested != 0 && !engine->stop_requested()) {
        HFT_WARN("shutdown signal received, stopping feed");
        std::fprintf(stderr, "\nshutdown signal received, stopping cleanly...\n");
        engine->request_stop();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  HFT_INFO("engine starting", "events", static_cast<double>(cfg.events), "order_quantity",
           static_cast<double>(cfg.engine.order_quantity));
  std::printf("feed: %s\nrunning...\n", feed->name());

  EngineStats stats;
  try {
    stats = engine->run(*feed);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: engine run failed: %s\n", e.what());
    watcher_done.store(true, std::memory_order_release);
    watcher.join();
    log.stop();
    return kExitRuntime;
  }

  watcher_done.store(true, std::memory_order_release);
  watcher.join();

  // --- graceful shutdown ---------------------------------------------------
  if (cfg.flatten_on_exit) {
    const std::uint64_t flattened = engine->flatten(stats);
    if (flattened > 0) {
      HFT_INFO("flattened inventory on shutdown", "orders", static_cast<double>(flattened));
      std::printf("\nflattened %llu position(s) on shutdown\n", (unsigned long long)flattened);
    }
  }

  HFT_INFO("engine stopped", "ticks", static_cast<double>(stats.ticks), "orders",
           static_cast<double>(stats.orders_sent), "rejects",
           static_cast<double>(stats.risk_rejects));

  // --- report --------------------------------------------------------------
  const OrderBook& book = engine->book();
  const PaperVenue& venue = engine->venue();

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
  std::printf("risk rejects        : %llu\n", (unsigned long long)stats.risk_rejects);
  std::printf("ring buffer drops   : %llu\n", (unsigned long long)stats.dropped_ticks);
  std::printf("wall time           : %.3f s\n", (double)stats.wall_ns / 1e9);
  std::printf("throughput          : %.0f ticks/sec\n", stats.ticks_per_second());

  std::printf("\n--- risk ---\n%s", engine->risk().summary().c_str());

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
  std::printf("      %-23s | %s\n", "bid px / qty", "ask px / qty");
  for (std::size_t i = 0; i < 5; ++i) {
    char lhs[48] = "";
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

  std::printf("\n--- paper trading ---\n");
  std::printf("fills               : %llu\n", (unsigned long long)venue.fill_count());
  std::printf("position %-11s: %lld\n", cfg.symbol.c_str(), (long long)venue.position(sym));
  std::printf("realized pnl        : %.2f\n", venue.realized_pnl());
  std::printf("fees paid           : %.2f\n", venue.fees_paid());
  if (bb != kNoPrice && ba != kNoPrice) {
    std::printf("equity (marked mid) : %.2f\n", venue.equity(sym, (bb + ba) / 2));
  }

  std::printf("\n--- latency (ns, one dev machine, not production hardware) ---\n");
  std::printf("%s", engine->latency().summary().c_str());

  // --- artefacts -----------------------------------------------------------
  int result = kExitOk;
  if (!cfg.out_dir.empty()) {
    struct Artefact {
      const char* name;
      bool ok;
    };
    const std::string metrics_path = join(cfg.out_dir, "metrics.json");
    const Artefact artefacts[] = {
        {"metrics.json", write_metrics_json(metrics_path, *engine, stats, symbols)},
        {"latency_summary.csv",
         engine->latency().write_summary_csv(join(cfg.out_dir, "latency_summary.csv"))},
        {"latency_histogram.csv",
         engine->latency().write_histogram_csv(join(cfg.out_dir, "latency_histogram.csv"))},
        {"fills.csv", venue.write_fills_csv(join(cfg.out_dir, "fills.csv"))},
        {"book_snapshot.csv",
         engine->write_book_snapshot_csv(join(cfg.out_dir, "book_snapshot.csv"),
                                         cfg.depth_levels)},
    };

    std::printf("\n--- artefacts (%s) ---\n", cfg.out_dir.c_str());
    for (const auto& a : artefacts) {
      std::printf("  %-24s %s\n", a.name, a.ok ? "ok" : "FAILED");
      if (!a.ok) {
        std::fprintf(stderr, "error: could not write %s into '%s' (does the directory exist?)\n",
                     a.name, cfg.out_dir.c_str());
        HFT_ERROR("artefact write failed");
        result = kExitIo;
      }
    }
  }

  if (log.dropped() > 0) {
    std::fprintf(stderr, "warning: %llu log record(s) dropped under load\n",
                 (unsigned long long)log.dropped());
  }
  log.stop();
  std::printf("\n");
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  // A trading engine must never die with an unexplained std::terminate.
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: unhandled exception: %s\n", e.what());
    return kExitRuntime;
  } catch (...) {
    std::fprintf(stderr, "fatal: unhandled non-standard exception\n");
    return kExitRuntime;
  }
}
