#include "hft/reconcile.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace hft {
namespace {

std::string trim(const std::string& s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return s.substr(b, e - b);
}

std::vector<std::string> split(const std::string& line, char sep) {
  std::vector<std::string> out;
  std::string field;
  std::istringstream in(line);
  while (std::getline(in, field, sep)) out.push_back(trim(field));
  return out;
}

bool parse_i64(const std::string& text, std::int64_t& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const long long v = std::strtoll(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return false;
  out = static_cast<std::int64_t>(v);
  return true;
}

bool parse_side(const std::string& text, Side& out) {
  std::string upper;
  upper.reserve(text.size());
  for (char c : text) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  if (upper == "BUY" || upper == "B" || upper == "BID") {
    out = Side::Buy;
    return true;
  }
  if (upper == "SELL" || upper == "S" || upper == "ASK" || upper == "OFFER") {
    out = Side::Sell;
    return true;
  }
  return false;
}

}  // namespace

const char* to_string(BreakKind k) {
  switch (k) {
    case BreakKind::OrphanAtVenue: return "ORPHAN_AT_VENUE";
    case BreakKind::MissingAtVenue: return "MISSING_AT_VENUE";
    case BreakKind::QuantityMismatch: return "QUANTITY_MISMATCH";
    case BreakKind::TermsMismatch: return "TERMS_MISMATCH";
    case BreakKind::PositionMismatch: return "POSITION_MISMATCH";
    default: return "UNKNOWN";
  }
}

std::size_t ReconciliationReport::count(BreakKind kind) const {
  std::size_t n = 0;
  for (const auto& d : discrepancies) {
    if (d.kind == kind) ++n;
  }
  return n;
}

std::string ReconciliationReport::summary() const {
  std::ostringstream out;
  if (!venue_available) {
    out << "venue state    : UNAVAILABLE";
    if (!error.empty()) out << " (" << error << ")";
    out << "\n";
    out << "  Nothing was compared. An unreachable venue is not a clean\n"
           "  reconciliation -- it is no reconciliation at all.\n";
    return out.str();
  }

  out << "venue state    : available\n";
  out << "open orders    : " << orders_ours << " ours, " << orders_theirs << " theirs, "
      << orders_agreed << " agreed\n";
  out << "discrepancies  : " << discrepancies.size() << "\n";
  if (discrepancies.empty()) {
    out << "  Our view and the venue's agree.\n";
    return out.str();
  }
  for (const auto& d : discrepancies) {
    out << "  " << to_string(d.kind);
    if (d.cl_ord_id != 0) out << " cl_ord_id=" << d.cl_ord_id;
    out << " symbol=" << d.symbol << " ours=" << d.ours << " theirs=" << d.theirs;
    if (!d.detail.empty()) out << "  " << d.detail;
    out << "\n";
  }
  if (count(BreakKind::OrphanAtVenue) > 0) {
    out << "  An orphaned order is live at the venue and untracked here. Cancel\n"
           "  it at the venue before this engine quotes again.\n";
  }
  return out.str();
}

ReconciliationReport reconcile(const RecoveredState& ours, const VenueSnapshot& theirs) {
  ReconciliationReport report;
  report.venue_available = theirs.available;
  if (!theirs.available) return report;

  report.orders_theirs = theirs.open_orders.size();

  // Index the venue's orders by our client order id. A venue order carrying no
  // client id cannot be matched to anything we sent, so it is an orphan by
  // definition -- and one we cannot even name.
  std::unordered_map<ClOrdId, const VenueOrder*> theirs_by_id;
  std::vector<const VenueOrder*> unidentified;
  for (const auto& vo : theirs.open_orders) {
    if (vo.cl_ord_id == 0) {
      unidentified.push_back(&vo);
      continue;
    }
    theirs_by_id.emplace(vo.cl_ord_id, &vo);
  }

  // Claimed by pointer rather than by id, so that two venue orders sharing one
  // client id do not both count as matched -- the second is itself a break.
  std::unordered_set<const VenueOrder*> claimed;
  for (const auto& our : ours.open_orders) {
    if (!is_working(our.state)) continue;
    ++report.orders_ours;
    const auto it = theirs_by_id.find(our.cl_ord_id);
    if (it == theirs_by_id.end()) {
      Discrepancy d;
      d.kind = BreakKind::MissingAtVenue;
      d.cl_ord_id = our.cl_ord_id;
      d.symbol = our.symbol;
      d.ours = our.leaves;
      d.theirs = 0;
      d.detail = std::string("we hold it ") + to_string(our.state) +
                 "; the venue is not resting it";
      report.discrepancies.push_back(std::move(d));
      continue;
    }
    claimed.insert(it->second);
    const VenueOrder& vo = *it->second;

    // Terms first. If the price or the side differs, the id refers to two
    // different orders and comparing their quantities is meaningless.
    if (vo.side != our.side || vo.price != our.price || vo.symbol != our.symbol) {
      Discrepancy d;
      d.kind = BreakKind::TermsMismatch;
      d.cl_ord_id = our.cl_ord_id;
      d.symbol = our.symbol;
      d.ours = our.price;
      d.theirs = vo.price;
      d.detail = std::string("ours ") + to_string(our.side) + " sym=" +
                 std::to_string(our.symbol) + ", theirs " + to_string(vo.side) +
                 " sym=" + std::to_string(vo.symbol);
      report.discrepancies.push_back(std::move(d));
      continue;
    }
    if (vo.leaves != our.leaves) {
      Discrepancy d;
      d.kind = BreakKind::QuantityMismatch;
      d.cl_ord_id = our.cl_ord_id;
      d.symbol = our.symbol;
      d.ours = our.leaves;
      d.theirs = vo.leaves;
      d.detail = "leaves differ; a fill report was probably lost";
      report.discrepancies.push_back(std::move(d));
      continue;
    }
    ++report.orders_agreed;
    report.agreed_open.push_back(vo);
  }

  // Anything the venue is resting that we did not claim above.
  for (const auto& vo : theirs.open_orders) {
    if (claimed.count(&vo) != 0) continue;
    Discrepancy d;
    d.kind = BreakKind::OrphanAtVenue;
    d.cl_ord_id = vo.cl_ord_id;
    d.symbol = vo.symbol;
    d.ours = 0;
    d.theirs = vo.leaves;
    if (vo.cl_ord_id == 0) {
      d.detail = "venue order carries no client id";
    } else if (theirs_by_id.count(vo.cl_ord_id) != 0 && theirs_by_id[vo.cl_ord_id] != &vo) {
      d.detail = "second venue order sharing one client id";
    } else {
      d.detail = "live at the venue, untracked here";
    }
    report.discrepancies.push_back(std::move(d));
  }

  // Positions. Compare across the union of both sides' symbols, because a
  // symbol we have no record of at all is exactly the one most worth checking.
  std::unordered_map<SymbolId, std::int64_t> theirs_pos;
  for (const auto& p : theirs.positions) theirs_pos[p.symbol] += p.position;

  std::unordered_set<SymbolId> symbols;
  for (SymbolId s = 0; s < ours.positions.size(); ++s) {
    if (ours.positions[s] != 0) symbols.insert(s);
  }
  for (const auto& kv : theirs_pos) {
    if (kv.second != 0) symbols.insert(kv.first);
  }
  std::vector<SymbolId> ordered(symbols.begin(), symbols.end());
  std::sort(ordered.begin(), ordered.end());
  for (SymbolId s : ordered) {
    const std::int64_t mine = ours.position(s);
    const auto it = theirs_pos.find(s);
    const std::int64_t yours = it == theirs_pos.end() ? 0 : it->second;
    if (mine == yours) continue;
    Discrepancy d;
    d.kind = BreakKind::PositionMismatch;
    d.symbol = s;
    d.ours = mine;
    d.theirs = yours;
    d.detail = "the venue is the authority; our replay is short by " +
               std::to_string(yours - mine);
    report.discrepancies.push_back(std::move(d));
  }

  return report;
}

// ------------------------------------------------------------------ file source

bool FileVenueStateSource::fetch(VenueSnapshot& out, std::string& error) {
  out = VenueSnapshot{};
  std::ifstream in(path_);
  if (!in) {
    error = "cannot read venue state file '" + path_ + "'";
    return false;
  }

  auto resolve = [&](const std::string& name, SymbolId& id) -> bool {
    if (instruments_ != nullptr) {
      const Instrument* found = instruments_->find(name);
      if (found != nullptr) {
        id = found->id;
        return true;
      }
    }
    // A bare number is accepted so the format stays usable without a registry
    // (tests, and venues that report numeric instrument ids).
    std::int64_t numeric = 0;
    if (parse_i64(name, numeric) && numeric >= 0) {
      id = static_cast<SymbolId>(numeric);
      return true;
    }
    return false;
  };

  std::string line;
  std::size_t line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    const std::string text = trim(line);
    if (text.empty() || text[0] == '#') continue;
    const std::vector<std::string> f = split(text, ',');
    const std::string kind = f.empty() ? std::string() : f[0];

    auto fail = [&](const std::string& why) {
      error = path_ + ":" + std::to_string(line_no) + ": " + why;
      out = VenueSnapshot{};
      return false;
    };

    if (kind == "ORDER") {
      if (f.size() < 9) return fail("ORDER needs 9 fields");
      VenueOrder vo;
      std::int64_t v = 0;
      if (!parse_i64(f[1], v) || v < 0) return fail("bad cl_ord_id");
      vo.cl_ord_id = static_cast<ClOrdId>(v);
      if (!parse_i64(f[2], v) || v < 0) return fail("bad venue_order_id");
      vo.venue_order_id = static_cast<OrderId>(v);
      if (!resolve(f[3], vo.symbol)) return fail("unknown symbol '" + f[3] + "'");
      if (!parse_side(f[4], vo.side)) return fail("bad side '" + f[4] + "'");
      if (!parse_i64(f[5], vo.price)) return fail("bad price");
      if (!parse_i64(f[6], vo.quantity) || vo.quantity <= 0) return fail("bad quantity");
      if (!parse_i64(f[7], vo.filled) || vo.filled < 0) return fail("bad filled");
      if (!parse_i64(f[8], vo.leaves) || vo.leaves < 0) return fail("bad leaves");
      if (vo.filled + vo.leaves > vo.quantity) {
        return fail("filled + leaves exceeds quantity");
      }
      out.open_orders.push_back(vo);
    } else if (kind == "POSITION") {
      if (f.size() < 3) return fail("POSITION needs 3 fields");
      VenuePosition vp;
      if (!resolve(f[1], vp.symbol)) return fail("unknown symbol '" + f[1] + "'");
      if (!parse_i64(f[2], vp.position)) return fail("bad position");
      out.positions.push_back(vp);
    } else {
      return fail("unknown record type '" + kind + "'");
    }
  }

  out.available = true;
  return true;
}

}  // namespace hft
