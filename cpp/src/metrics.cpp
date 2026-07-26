#include "hft/metrics.hpp"

#include <fstream>
#include <sstream>

namespace hft {
namespace {

// Minimal JSON string escaping. Symbol names are the only user-controlled text
// that reaches this file, but escaping it properly costs nothing and means a
// symbol containing a quote cannot produce a corrupt document.
std::string escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

void write_histogram(std::ostringstream& os, const char* name, const LatencyHistogram& h,
                     bool last) {
  os << "      \"" << name << "\": {"
     << "\"count\": " << h.count() << ", "
     << "\"min_ns\": " << h.min() << ", "
     << "\"p50_ns\": " << h.percentile(50.0) << ", "
     << "\"p90_ns\": " << h.percentile(90.0) << ", "
     << "\"p99_ns\": " << h.percentile(99.0) << ", "
     << "\"p999_ns\": " << h.percentile(99.9) << ", "
     << "\"p9999_ns\": " << h.percentile(99.99) << ", "
     << "\"max_ns\": " << h.max() << ", "
     << "\"mean_ns\": " << h.mean() << "}" << (last ? "\n" : ",\n");
}

}  // namespace

std::string metrics_json(const Engine& engine, const EngineStats& stats,
                         const SymbolTable& symbols) {
  const RiskManager& risk = engine.risk();
  const PaperVenue& venue = engine.venue();
  const OrderBook& book = engine.book();
  const LatencyRecorder& lat = engine.latency();

  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(4);

  os << "{\n";
  os << "  \"schema_version\": 1,\n";

  // --- run ------------------------------------------------------------------
  os << "  \"run\": {\n"
     << "    \"ticks\": " << stats.ticks << ",\n"
     << "    \"wall_seconds\": " << static_cast<double>(stats.wall_ns) / 1e9 << ",\n"
     << "    \"ticks_per_second\": " << stats.ticks_per_second() << ",\n"
     << "    \"threaded\": " << (engine.config().threaded ? "true" : "false") << ",\n"
     << "    \"dropped_ticks\": " << stats.dropped_ticks << "\n"
     << "  },\n";

  // --- book -----------------------------------------------------------------
  os << "  \"book\": {\n"
     << "    \"adds\": " << stats.adds << ",\n"
     << "    \"cancels\": " << stats.cancels << ",\n"
     << "    \"cancels_rejected\": " << stats.cancel_rejected << ",\n"
     << "    \"aggressive_orders\": " << stats.aggressive_orders << ",\n"
     << "    \"trades_matched\": " << stats.book_trades << ",\n"
     << "    \"resting_orders\": " << book.live_order_count() << ",\n"
     << "    \"self_match_preventions\": " << book.self_match_preventions() << ",\n";
  const Price bb = book.best_bid();
  const Price ba = book.best_ask();
  if (bb != kNoPrice) {
    os << "    \"best_bid\": " << price_to_double(bb) << ",\n"
       << "    \"best_bid_quantity\": " << book.best_bid_quantity() << ",\n";
  } else {
    os << "    \"best_bid\": null,\n    \"best_bid_quantity\": 0,\n";
  }
  if (ba != kNoPrice) {
    os << "    \"best_ask\": " << price_to_double(ba) << ",\n"
       << "    \"best_ask_quantity\": " << book.best_ask_quantity() << "\n";
  } else {
    os << "    \"best_ask\": null,\n    \"best_ask_quantity\": 0\n";
  }
  os << "  },\n";

  // --- trading --------------------------------------------------------------
  os << "  \"trading\": {\n"
     << "    \"signals\": " << stats.signals << ",\n"
     << "    \"orders_sent\": " << stats.orders_sent << ",\n"
     << "    \"risk_rejects\": " << stats.risk_rejects << ",\n"
     << "    \"flatten_orders\": " << stats.flatten_orders << ",\n"
     << "    \"fills\": " << venue.fill_count() << ",\n"
     << "    \"realized_pnl\": " << venue.realized_pnl() << ",\n"
     << "    \"fees_paid\": " << venue.fees_paid() << ",\n"
     << "    \"positions\": {";
  bool first = true;
  for (const auto& kv : venue.positions()) {
    if (!first) os << ", ";
    os << "\"" << escape(symbols.name(kv.first)) << "\": " << kv.second;
    first = false;
  }
  os << "}\n  },\n";

  // --- risk -----------------------------------------------------------------
  os << "  \"risk\": {\n"
     << "    \"enabled\": " << (risk.limits().enabled ? "true" : "false") << ",\n"
     << "    \"halted\": " << (risk.halted() ? "true" : "false") << ",\n"
     << "    \"halt_reason\": \"" << to_string(risk.halt_reason()) << "\",\n"
     << "    \"accepted_orders\": " << risk.accepted_orders() << ",\n"
     << "    \"rejected_orders\": " << risk.rejected_orders() << ",\n"
     << "    \"gross_position\": " << risk.gross_position() << ",\n"
     << "    \"peak_equity\": " << risk.peak_equity() << ",\n"
     << "    \"drawdown\": " << risk.drawdown() << ",\n"
     << "    \"rejects_by_reason\": {\n";
  bool first_reject = true;
  for (std::size_t i = 1; i < static_cast<std::size_t>(RejectReason::kCount); ++i) {
    const RejectReason reason = static_cast<RejectReason>(i);
    if (!first_reject) os << ",\n";
    os << "      \"" << to_string(reason) << "\": " << risk.rejects(reason);
    first_reject = false;
  }
  os << "\n    },\n";
  os << "    \"limits\": {\n"
     << "      \"max_order_quantity\": " << risk.limits().max_order_quantity << ",\n"
     << "      \"max_order_notional\": " << risk.limits().max_order_notional << ",\n"
     << "      \"max_position_per_symbol\": " << risk.limits().max_position_per_symbol << ",\n"
     << "      \"max_gross_position\": " << risk.limits().max_gross_position << ",\n"
     << "      \"price_collar_bps\": " << risk.limits().price_collar_bps << ",\n"
     << "      \"max_orders_per_second\": " << risk.limits().max_orders_per_second << ",\n"
     << "      \"max_daily_orders\": " << risk.limits().max_daily_orders << ",\n"
     << "      \"max_drawdown\": " << risk.limits().max_drawdown << "\n"
     << "    }\n"
     << "  },\n";

  // --- latency --------------------------------------------------------------
  os << "  \"latency\": {\n"
     << "    \"note\": \"nanoseconds, measured on a single general-purpose dev "
        "machine with no core pinning; not production hardware\",\n"
     << "    \"stages\": {\n";
  write_histogram(os, "ingest_to_book", lat.ingest_to_book, false);
  write_histogram(os, "book_update", lat.book_update, false);
  write_histogram(os, "signal_compute", lat.signal_compute, false);
  write_histogram(os, "risk_check", lat.risk_check, false);
  write_histogram(os, "order_round_trip", lat.order_round_trip, false);
  write_histogram(os, "tick_to_order", lat.tick_to_order, true);
  os << "    }\n  }\n";

  os << "}\n";
  return os.str();
}

bool write_metrics_json(const std::string& path, const Engine& engine, const EngineStats& stats,
                        const SymbolTable& symbols) {
  std::ofstream f(path);
  if (!f) return false;
  f << metrics_json(engine, stats, symbols);
  f.flush();
  return static_cast<bool>(f);
}

}  // namespace hft
