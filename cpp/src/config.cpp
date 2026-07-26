#include "hft/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace hft {
namespace {

std::string trim(const std::string& s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

bool parse_bool(const std::string& v, bool& out) {
  std::string t;
  for (char c : v) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (t == "1" || t == "true" || t == "yes" || t == "on") { out = true; return true; }
  if (t == "0" || t == "false" || t == "no" || t == "off") { out = false; return true; }
  return false;
}

// Strict numeric parsing: the whole token must be consumed. "10abc" and "" are
// errors, not 10. Silently accepting garbage in a risk limit is unacceptable.
bool parse_i64(const std::string& v, long long& out) {
  if (v.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const long long parsed = std::strtoll(v.c_str(), &end, 10);
  if (errno != 0 || end == v.c_str() || *end != '\0') return false;
  out = parsed;
  return true;
}

bool parse_u64(const std::string& v, unsigned long long& out) {
  long long signed_value = 0;
  if (!parse_i64(v, signed_value) || signed_value < 0) return false;
  out = static_cast<unsigned long long>(signed_value);
  return true;
}

bool parse_double(const std::string& v, double& out) {
  if (v.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double parsed = std::strtod(v.c_str(), &end);
  if (errno != 0 || end == v.c_str() || *end != '\0') return false;
  out = parsed;
  return true;
}

}  // namespace

const std::vector<std::string>& config_keys() {
  static const std::vector<std::string> keys = {
      // feed
      "symbol", "events", "seed", "replay_path",
      // strategy
      "fast_window", "slow_window", "order_quantity",
      // venue
      "slippage_bps", "fee_bps", "cross_book",
      // book / engine
      "min_price", "max_price", "ring_capacity", "threaded", "record_curve",
      // risk
      "risk_enabled", "max_order_quantity", "max_order_notional",
      "max_position_per_symbol", "max_gross_position", "price_collar_bps",
      "max_orders_per_second", "max_daily_orders", "max_drawdown",
      // order management
      "max_open_orders", "ack_timeout_ms", "order_history",
      // market data session health
      "feed_require_sequence", "feed_halt_on_gap", "feed_tolerated_gap", "feed_stale_ms",
      // durability
      "journal_path", "journal_sync", "journal_sync_interval", "allow_unclean_start",
      // output / ops
      "out_dir", "depth_levels", "log_level", "flatten_on_exit",
  };
  return keys;
}

bool apply_config_setting(const std::string& key, const std::string& value, AppConfig& cfg,
                          std::string& error) {
  long long i = 0;
  unsigned long long u = 0;
  double d = 0.0;
  bool b = false;

  auto need_i64 = [&](const char* what) {
    if (!parse_i64(value, i)) {
      error = std::string(what) + " expects an integer, got '" + value + "'";
      return false;
    }
    return true;
  };
  auto need_u64 = [&](const char* what) {
    if (!parse_u64(value, u)) {
      error = std::string(what) + " expects a non-negative integer, got '" + value + "'";
      return false;
    }
    return true;
  };
  auto need_double = [&](const char* what) {
    if (!parse_double(value, d)) {
      error = std::string(what) + " expects a number, got '" + value + "'";
      return false;
    }
    return true;
  };
  auto need_bool = [&](const char* what) {
    if (!parse_bool(value, b)) {
      error = std::string(what) + " expects true/false, got '" + value + "'";
      return false;
    }
    return true;
  };
  auto need_positive = [&](const char* what, long long v) {
    if (v <= 0) {
      error = std::string(what) + " must be positive, got " + std::to_string(v);
      return false;
    }
    return true;
  };
  auto need_non_negative_d = [&](const char* what, double v) {
    if (v < 0.0) {
      error = std::string(what) + " must not be negative, got " + value;
      return false;
    }
    return true;
  };

  // --- feed -----------------------------------------------------------------
  if (key == "symbol") {
    if (value.empty()) { error = "symbol must not be empty"; return false; }
    cfg.symbol = value;
  } else if (key == "events") {
    if (!need_u64("events")) return false;
    cfg.events = static_cast<std::size_t>(u);
  } else if (key == "seed") {
    if (!need_u64("seed")) return false;
    cfg.seed = u;
  } else if (key == "replay_path") {
    cfg.replay_path = value;

    // --- strategy -----------------------------------------------------------
  } else if (key == "fast_window") {
    if (!need_i64("fast_window") || !need_positive("fast_window", i)) return false;
    cfg.engine.fast_window = static_cast<std::size_t>(i);
  } else if (key == "slow_window") {
    if (!need_i64("slow_window") || !need_positive("slow_window", i)) return false;
    cfg.engine.slow_window = static_cast<std::size_t>(i);
  } else if (key == "order_quantity") {
    if (!need_i64("order_quantity") || !need_positive("order_quantity", i)) return false;
    cfg.engine.order_quantity = static_cast<Quantity>(i);

    // --- venue --------------------------------------------------------------
  } else if (key == "slippage_bps") {
    if (!need_double("slippage_bps") || !need_non_negative_d("slippage_bps", d)) return false;
    cfg.engine.slippage_bps = d;
  } else if (key == "fee_bps") {
    if (!need_double("fee_bps") || !need_non_negative_d("fee_bps", d)) return false;
    cfg.engine.fee_bps = d;
  } else if (key == "cross_book") {
    if (!need_bool("cross_book")) return false;
    cfg.engine.cross_book = b;

    // --- book / engine ------------------------------------------------------
  } else if (key == "min_price") {
    if (!need_i64("min_price") || !need_positive("min_price", i)) return false;
    cfg.engine.min_price = static_cast<Price>(i);
  } else if (key == "max_price") {
    if (!need_i64("max_price") || !need_positive("max_price", i)) return false;
    cfg.engine.max_price = static_cast<Price>(i);
  } else if (key == "ring_capacity") {
    if (!need_i64("ring_capacity") || !need_positive("ring_capacity", i)) return false;
    cfg.engine.ring_capacity = static_cast<std::size_t>(i);
  } else if (key == "threaded") {
    if (!need_bool("threaded")) return false;
    cfg.engine.threaded = b;
  } else if (key == "record_curve") {
    if (!need_bool("record_curve")) return false;
    cfg.engine.record_curve = b;

    // --- risk ---------------------------------------------------------------
  } else if (key == "risk_enabled") {
    if (!need_bool("risk_enabled")) return false;
    cfg.engine.risk.enabled = b;
  } else if (key == "max_order_quantity") {
    if (!need_i64("max_order_quantity") || !need_positive("max_order_quantity", i)) return false;
    cfg.engine.risk.max_order_quantity = static_cast<Quantity>(i);
  } else if (key == "max_order_notional") {
    if (!need_double("max_order_notional") || !need_non_negative_d("max_order_notional", d)) {
      return false;
    }
    cfg.engine.risk.max_order_notional = d;
  } else if (key == "max_position_per_symbol") {
    if (!need_i64("max_position_per_symbol") || !need_positive("max_position_per_symbol", i)) {
      return false;
    }
    cfg.engine.risk.max_position_per_symbol = i;
  } else if (key == "max_gross_position") {
    if (!need_i64("max_gross_position") || !need_positive("max_gross_position", i)) return false;
    cfg.engine.risk.max_gross_position = i;
  } else if (key == "price_collar_bps") {
    if (!need_double("price_collar_bps") || !need_non_negative_d("price_collar_bps", d)) {
      return false;
    }
    cfg.engine.risk.price_collar_bps = d;
  } else if (key == "max_orders_per_second") {
    if (!need_u64("max_orders_per_second")) return false;
    cfg.engine.risk.max_orders_per_second = static_cast<std::uint32_t>(
        std::min<unsigned long long>(u, 0xFFFFFFFFULL));
  } else if (key == "max_daily_orders") {
    if (!need_u64("max_daily_orders")) return false;
    cfg.engine.risk.max_daily_orders = u;
  } else if (key == "max_drawdown") {
    if (!need_double("max_drawdown") || !need_non_negative_d("max_drawdown", d)) return false;
    cfg.engine.risk.max_drawdown = d;

    // --- order management ---------------------------------------------------
  } else if (key == "max_open_orders") {
    if (!need_i64("max_open_orders") || !need_positive("max_open_orders", i)) return false;
    cfg.engine.oms.max_open_orders = static_cast<std::size_t>(i);
  } else if (key == "ack_timeout_ms") {
    // Milliseconds in the config, nanoseconds internally: nobody wants to write
    // an ack timeout with nine zeroes in it.
    if (!need_i64("ack_timeout_ms") || !need_positive("ack_timeout_ms", i)) return false;
    cfg.engine.oms.ack_timeout_ns = static_cast<Nanos>(i) * 1'000'000;
  } else if (key == "order_history") {
    if (!need_i64("order_history") || !need_positive("order_history", i)) return false;
    cfg.engine.oms.retired_history = static_cast<std::size_t>(i);

    // --- market data session health -----------------------------------------
  } else if (key == "feed_require_sequence") {
    if (!need_bool("feed_require_sequence")) return false;
    cfg.engine.feed_health.require_sequence = b;
  } else if (key == "feed_halt_on_gap") {
    if (!need_bool("feed_halt_on_gap")) return false;
    cfg.engine.feed_health.halt_on_gap = b;
  } else if (key == "feed_tolerated_gap") {
    if (!need_u64("feed_tolerated_gap")) return false;
    cfg.engine.feed_health.tolerated_gap = u;
  } else if (key == "feed_stale_ms") {
    // 0 disables the watchdog, so unlike the other durations this one is
    // allowed to be zero -- but not negative.
    if (!need_u64("feed_stale_ms")) return false;
    cfg.engine.feed_health.stale_after_ns = static_cast<Nanos>(u) * 1'000'000;

    // --- durability ---------------------------------------------------------
  } else if (key == "journal_path") {
    cfg.journal_path = value;
  } else if (key == "journal_sync") {
    SyncPolicy policy;
    if (!parse_sync_policy(value, policy)) {
      error = "journal_sync expects on_write/always/interval, got '" + value + "'";
      return false;
    }
    cfg.journal_sync = policy;
  } else if (key == "journal_sync_interval") {
    if (!need_i64("journal_sync_interval") || !need_positive("journal_sync_interval", i)) {
      return false;
    }
    cfg.journal_sync_interval = static_cast<std::uint64_t>(i);
  } else if (key == "allow_unclean_start") {
    if (!need_bool("allow_unclean_start")) return false;
    cfg.allow_unclean_start = b;

    // --- output / ops -------------------------------------------------------
  } else if (key == "out_dir") {
    cfg.out_dir = value;
  } else if (key == "depth_levels") {
    if (!need_i64("depth_levels") || !need_positive("depth_levels", i)) return false;
    cfg.depth_levels = static_cast<std::size_t>(i);
  } else if (key == "log_level") {
    LogLevel lvl;
    if (!parse_log_level(value, lvl)) {
      error = "log_level expects trace/debug/info/warn/error/off, got '" + value + "'";
      return false;
    }
    cfg.log_level = lvl;
  } else if (key == "flatten_on_exit") {
    if (!need_bool("flatten_on_exit")) return false;
    cfg.flatten_on_exit = b;

  } else {
    error = "unknown setting '" + key + "'";
    return false;
  }
  return true;
}

bool load_config_file(const std::string& path, AppConfig& cfg, std::vector<ConfigError>& errors) {
  std::ifstream f(path);
  if (!f) {
    errors.push_back(ConfigError{0, "cannot open config file: " + path});
    return false;
  }

  std::string line;
  std::size_t lineno = 0;
  const std::size_t errors_before = errors.size();

  while (std::getline(f, line)) {
    ++lineno;
    // Strip comments, but only when '#' starts a token -- so a value can
    // legitimately contain one.
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;

    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) {
      errors.push_back(ConfigError{lineno, "expected 'key = value', got: " + line});
      continue;
    }

    const std::string key = trim(line.substr(0, eq));
    const std::string value = trim(line.substr(eq + 1));
    if (key.empty()) {
      errors.push_back(ConfigError{lineno, "empty key"});
      continue;
    }

    std::string error;
    if (!apply_config_setting(key, value, cfg, error)) {
      errors.push_back(ConfigError{lineno, error});
    }
  }

  return errors.size() == errors_before;
}

bool validate_config(const AppConfig& cfg, std::vector<ConfigError>& errors) {
  const std::size_t before = errors.size();

  if (cfg.engine.fast_window >= cfg.engine.slow_window) {
    errors.push_back(ConfigError{
        0, "fast_window (" + std::to_string(cfg.engine.fast_window) + ") must be smaller than " +
               "slow_window (" + std::to_string(cfg.engine.slow_window) + ")"});
  }
  if (cfg.engine.min_price >= cfg.engine.max_price) {
    errors.push_back(ConfigError{0, "min_price must be below max_price"});
  }
  if (cfg.engine.max_price - cfg.engine.min_price + 1 > 262144) {
    errors.push_back(ConfigError{
        0, "price band exceeds 262144 ticks, which is the order book's addressable range"});
  }
  if (cfg.engine.risk.enabled &&
      cfg.engine.order_quantity > cfg.engine.risk.max_order_quantity) {
    errors.push_back(ConfigError{
        0, "order_quantity exceeds max_order_quantity: every order would be rejected"});
  }
  if (cfg.engine.risk.enabled &&
      cfg.engine.risk.max_position_per_symbol > cfg.engine.risk.max_gross_position) {
    errors.push_back(ConfigError{
        0, "max_position_per_symbol exceeds max_gross_position: the gross limit can never bind"});
  }
  if (cfg.events == 0 && cfg.replay_path.empty()) {
    errors.push_back(ConfigError{0, "events is 0 and no replay_path is set: nothing to do"});
  }

  return errors.size() == before;
}

std::string describe_config(const AppConfig& cfg) {
  std::ostringstream os;
  os << "  symbol                  = " << cfg.symbol << "\n"
     << "  events                  = " << cfg.events << "\n"
     << "  seed                    = " << cfg.seed << "\n"
     << "  replay_path             = " << (cfg.replay_path.empty() ? "(none)" : cfg.replay_path)
     << "\n"
     << "  fast_window             = " << cfg.engine.fast_window << "\n"
     << "  slow_window             = " << cfg.engine.slow_window << "\n"
     << "  order_quantity          = " << cfg.engine.order_quantity << "\n"
     << "  slippage_bps            = " << cfg.engine.slippage_bps << "\n"
     << "  fee_bps                 = " << cfg.engine.fee_bps << "\n"
     << "  cross_book              = " << (cfg.engine.cross_book ? "true" : "false") << "\n"
     << "  min_price               = " << cfg.engine.min_price << "\n"
     << "  max_price               = " << cfg.engine.max_price << "\n"
     << "  ring_capacity           = " << cfg.engine.ring_capacity << "\n"
     << "  threaded                = " << (cfg.engine.threaded ? "true" : "false") << "\n"
     << "  record_curve            = " << (cfg.engine.record_curve ? "true" : "false") << "\n"
     << "  risk_enabled            = " << (cfg.engine.risk.enabled ? "true" : "false") << "\n"
     << "  max_order_quantity      = " << cfg.engine.risk.max_order_quantity << "\n"
     << "  max_order_notional      = " << cfg.engine.risk.max_order_notional << "\n"
     << "  max_position_per_symbol = " << cfg.engine.risk.max_position_per_symbol << "\n"
     << "  max_gross_position      = " << cfg.engine.risk.max_gross_position << "\n"
     << "  price_collar_bps        = " << cfg.engine.risk.price_collar_bps << "\n"
     << "  max_orders_per_second   = " << cfg.engine.risk.max_orders_per_second << "\n"
     << "  max_daily_orders        = " << cfg.engine.risk.max_daily_orders << "\n"
     << "  max_drawdown            = " << cfg.engine.risk.max_drawdown << "\n"
     << "  max_open_orders         = " << cfg.engine.oms.max_open_orders << "\n"
     << "  ack_timeout_ms          = " << (cfg.engine.oms.ack_timeout_ns / 1'000'000) << "\n"
     << "  order_history           = " << cfg.engine.oms.retired_history << "\n"
     << "  feed_require_sequence   = "
     << (cfg.engine.feed_health.require_sequence ? "true" : "false") << "\n"
     << "  feed_halt_on_gap        = "
     << (cfg.engine.feed_health.halt_on_gap ? "true" : "false") << "\n"
     << "  feed_tolerated_gap      = " << cfg.engine.feed_health.tolerated_gap << "\n"
     << "  feed_stale_ms           = " << (cfg.engine.feed_health.stale_after_ns / 1'000'000)
     << "\n"
     << "  journal_path            = "
     << (cfg.journal_path.empty() ? "(none -- state is lost on crash)" : cfg.journal_path) << "\n"
     << "  journal_sync            = " << to_string(cfg.journal_sync) << "\n"
     << "  journal_sync_interval   = " << cfg.journal_sync_interval << "\n"
     << "  allow_unclean_start     = " << (cfg.allow_unclean_start ? "true" : "false") << "\n"
     << "  out_dir                 = " << (cfg.out_dir.empty() ? "(none)" : cfg.out_dir) << "\n"
     << "  depth_levels            = " << cfg.depth_levels << "\n"
     << "  log_level               = " << to_string(cfg.log_level) << "\n"
     << "  flatten_on_exit         = " << (cfg.flatten_on_exit ? "true" : "false") << "\n";
  return os.str();
}

}  // namespace hft
