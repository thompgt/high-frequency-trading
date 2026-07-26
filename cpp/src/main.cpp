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

#include <algorithm>
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
// The previous session left state that needs a human decision before trading
// resumes. Distinct from every other failure so a supervisor does not simply
// restart into the same situation.
constexpr int kExitUnclean = 6;

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
      "  --journal FILE        append every order and fill here, for crash recovery\n"
      "  --recover FILE        replay a journal, print what it recovers, and exit\n"
      "  --allow-unclean-start start even if the last session did not exit cleanly\n"
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
               std::string& replay_out, std::size_t& replay_out_events, std::string& recover_path,
               bool& exit_now, int& exit_code) {
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
    } else if (a == "--journal") {
      overrides.emplace_back("journal_path", need("--journal"));
    } else if (a == "--recover") {
      recover_path = need("--recover");
    } else if (a == "--allow-unclean-start") {
      overrides.emplace_back("allow_unclean_start", "true");
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
  std::string recover_path;
  std::size_t replay_out_events = 200'000;
  bool exit_now = false;
  int exit_code = kExitOk;

  if (!parse_cli(argc, argv, cfg, config_path, replay_out, replay_out_events, recover_path,
                 exit_now, exit_code)) {
    return exit_code;
  }
  if (exit_now) return exit_code;

  // --recover is an inspection mode: say what the journal contains and stop.
  // Reading a journal must never be gated on being able to start the engine.
  if (!recover_path.empty()) {
    const RecoveredState state = recover_from_journal(recover_path);
    std::printf("\n=== journal recovery: %s ===\n\n%s\n", recover_path.c_str(),
                state.summary().c_str());
    return state.ok ? kExitOk : kExitIo;
  }

  // With no `instrument` lines configured, fall back to the single-symbol
  // setup: one instrument named by `symbol` over the min/max price band. This
  // is what every existing config gets, and it keeps the simple case simple.
  if (cfg.engine.instruments.empty()) {
    Instrument only;
    only.symbol = cfg.symbol;
    only.min_price = cfg.engine.min_price;
    only.max_price = cfg.engine.max_price;
    std::string error;
    if (!cfg.engine.instruments.add(only, error)) {
      std::fprintf(stderr, "error: %s\n", error.c_str());
      return kExitConfig;
    }
  }

  // The symbol table mirrors the registry so ids agree everywhere. Interning
  // in registry order is what makes SymbolId usable as a direct index.
  SymbolTable symbols;
  for (const auto& instrument : cfg.engine.instruments.all()) {
    symbols.intern(instrument.symbol);
  }
  // One synthetic stream per instrument, each seeded distinctly so adding an
  // instrument does not change the order flow the others see.
  std::vector<SyntheticFeed::Params> feed_params;
  for (const auto& instrument : cfg.engine.instruments.all()) {
    SyntheticFeed::Params p;
    p.symbol = instrument.id;
    p.seed = cfg.seed + instrument.id * 0x9E3779B97F4A7C15ULL;
    p.total_events = cfg.events / cfg.engine.instruments.size();
    p.min_price = instrument.min_price;
    p.max_price = instrument.max_price;
    p.start_price = (instrument.min_price + instrument.max_price) / 2;
    // Generated prices must obey the instrument's contract or the engine will
    // (correctly) reject nearly everything the feed produces.
    p.tick_size = instrument.tick_size;
    // Spread orders over the same *number of price levels* regardless of tick
    // size, so a 25-tick instrument gets real depth rather than two levels.
    p.tick_range = static_cast<Price>(8) * instrument.tick_size;
    const Price span = instrument.max_price - instrument.min_price;
    if (p.tick_range * 4 > span) p.tick_range = std::max<Price>(instrument.tick_size, span / 8);
    feed_params.push_back(p);
  }
  SyntheticFeed::Params fp = feed_params[0];

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

  // --- recovery ------------------------------------------------------------
  // Before anything else can trade: find out what the last session left
  // behind. The venue still remembers our position and our resting orders even
  // though we do not, and quoting on top of them is how a restart turns a
  // crash into a loss.
  Journal journal;
  ClOrdId recovered_next_cl_ord_id = 1;
  if (!cfg.journal_path.empty()) {
    const RecoveredState recovered = recover_from_journal(cfg.journal_path);
    recovered_next_cl_ord_id = recovered.next_cl_ord_id + 1;
    if (!recovered.ok) {
      std::fprintf(stderr, "error: %s\n", recovered.error.c_str());
      log.stop();
      return kExitIo;
    }
    if (recovered.records_read > 0) {
      std::printf("\n--- recovered from %s ---\n%s", cfg.journal_path.c_str(),
                  recovered.summary().c_str());
    }

    // Fail closed. An unclean shutdown or a possibly-live order is a state a
    // human has to sign off on, so it exits with its own code rather than
    // letting a supervisor restart straight back into it.
    const bool needs_attention =
        (recovered.records_read > 0 && !recovered.clean_shutdown) ||
        !recovered.open_orders.empty() || recovered.corrupt_records > 0;
    if (needs_attention && !cfg.allow_unclean_start) {
      std::fprintf(stderr,
                   "\nerror: refusing to start -- the previous session left state that needs "
                   "reconciling:\n"
                   "  unclean shutdown : %s\n"
                   "  open orders      : %zu (may still be live at the venue)\n"
                   "  corrupt records  : %llu\n"
                   "Reconcile against the venue, then restart with "
                   "--allow-unclean-start.\n",
                   recovered.clean_shutdown ? "no" : "yes", recovered.open_orders.size(),
                   (unsigned long long)recovered.corrupt_records);
      HFT_ERROR("refusing to start after an unclean shutdown");
      log.stop();
      return kExitUnclean;
    }

    Journal::Config jcfg;
    jcfg.path = cfg.journal_path;
    jcfg.sync = cfg.journal_sync;
    jcfg.sync_interval = cfg.journal_sync_interval;
    jcfg.append = true;
    if (!journal.open(jcfg)) {
      std::fprintf(stderr, "error: %s\n", journal.error().c_str());
      log.stop();
      return kExitIo;
    }
    journal.record_session_start(now_ns());
  }

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
  } else if (feed_params.size() > 1) {
    feed = std::make_unique<MultiSymbolFeed>(feed_params);
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
  if (journal.is_open()) {
    engine->set_journal(&journal);
    // Continue the client order id sequence past everything the journal has
    // seen. Restarting ids at 1 would have a venue reject the duplicates and
    // would make the journal itself unreplayable, since one id would refer to
    // two different orders.
    engine->oms().set_next_id(recovered_next_cl_ord_id);
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

  // Checkpoint, then mark the session closed. The checkpoint carries the PnL
  // that cannot be rebuilt from fills alone; the end marker is what tells the
  // next start there is nothing to reconcile.
  if (journal.is_open()) {
    for (const auto& kv : engine->venue().positions()) {
      journal.record_checkpoint(kv.first, kv.second, engine->venue().realized_pnl(),
                                engine->venue().fees_paid(), now_ns());
    }
    journal.record_session_end(now_ns());
    journal.close();
  }

  // --- report --------------------------------------------------------------
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
  std::printf("untracked rejects   : %llu  (no OMS slot; never sent)\n",
              (unsigned long long)stats.untracked_rejects);
  std::printf("ring buffer drops   : %llu\n", (unsigned long long)stats.dropped_ticks);
  std::printf("wall time           : %.3f s\n", (double)stats.wall_ns / 1e9);
  std::printf("throughput          : %.0f ticks/sec\n", stats.ticks_per_second());

  std::printf("\n--- market data session ---\n%s", engine->feed_monitor().summary().c_str());
  if (engine->feed_monitor().stats().gaps > 0) {
    std::fprintf(stderr,
                 "warning: %llu sequence gap(s), %llu message(s) lost -- the book was built "
                 "from an incomplete stream\n",
                 (unsigned long long)engine->feed_monitor().stats().gaps,
                 (unsigned long long)engine->feed_monitor().stats().messages_missing);
    HFT_ERROR("market data gaps",
              "gaps", static_cast<double>(engine->feed_monitor().stats().gaps));
  }

  std::printf("\n--- orders ---\n%s", engine->oms().summary().c_str());
  if (engine->oms().stats().breaks() > 0) {
    // A break means our record of an order and the venue's have diverged.
    // Never let that scroll past as just another counter.
    std::fprintf(stderr,
                 "warning: %llu order reconciliation break(s) -- our view and the venue's "
                 "do not agree; investigate before trading again\n",
                 (unsigned long long)engine->oms().stats().breaks());
    HFT_ERROR("order reconciliation breaks",
              "count", static_cast<double>(engine->oms().stats().breaks()));
  }

  std::printf("\n--- risk ---\n%s", engine->risk().summary().c_str());

  // One section per instrument: with several books, a single merged view would
  // be meaningless.
  double marked_equity = venue.realized_pnl();
  for (const auto& instrument : engine->instruments().all()) {
    const OrderBook& book = engine->book(instrument.id);
    std::printf("\n--- final book: %s ---\n", instrument.symbol.c_str());
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
    std::printf("position            : %lld\n", (long long)venue.position(instrument.id));

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

    // Mark each instrument's inventory at its own mid. Marking one product at
    // another's price is how a book that is actually flat shows a profit.
    if (bb != kNoPrice && ba != kNoPrice && venue.position(instrument.id) != 0) {
      marked_equity += venue.equity(instrument.id, (bb + ba) / 2) - venue.realized_pnl();
    }
  }

  std::printf("\n--- paper trading ---\n");
  std::printf("fills               : %llu\n", (unsigned long long)venue.fill_count());
  std::printf("realized pnl        : %.2f\n", venue.realized_pnl());
  std::printf("fees paid           : %.2f\n", venue.fees_paid());
  std::printf("equity (marked mid) : %.2f\n", marked_equity);

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
