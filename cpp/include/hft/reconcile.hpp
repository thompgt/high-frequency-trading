// Startup reconciliation: comparing what we think we have against what the
// venue says we have.
//
// Why this exists
// ---------------
// Journal recovery answers "what did *we* last write down?". That is only half
// of the question a restart has to answer. The venue is the authority on what
// is actually resting on its book and what position we actually hold, and the
// two views can differ for reasons that are entirely normal:
//
//   * The engine died between sending an order and journalling the response,
//     so we have an order at PendingNew that the venue filled seconds ago.
//   * The engine died before the order left the socket, so we have an order
//     the venue has never heard of.
//   * A journal record was lost to corruption, so our position is stale.
//
// Only one of those differences is dangerous in the way that matters most: an
// order live at the venue that we are not tracking. We cannot cancel it, it is
// not in any risk calculation, and it can fill at any moment. That is the case
// this module exists to surface, and it is why a restart that cannot reach the
// venue is not the same thing as a restart that reconciled clean.
//
// What this is not
// ----------------
// This does not *repair* anything. It classifies and reports. Automatically
// cancelling an order you did not expect, or silently adopting the venue's
// position as truth, are decisions with real money attached, and an engine
// that makes them quietly at 06:00 is worse than one that stops and says what
// it found.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hft/instrument.hpp"
#include "hft/journal.hpp"
#include "hft/oms.hpp"
#include "hft/types.hpp"

namespace hft {

// One order as the venue reports it. The fields mirror what an order-status
// response or a drop copy carries; nothing here is our own bookkeeping.
struct VenueOrder {
  ClOrdId cl_ord_id = 0;
  OrderId venue_order_id = 0;
  SymbolId symbol = 0;
  Side side = Side::Buy;
  Price price = 0;
  Quantity quantity = 0;  // original order quantity
  Quantity filled = 0;    // cumulative executed
  Quantity leaves = 0;    // still resting
};

struct VenuePosition {
  SymbolId symbol = 0;
  std::int64_t position = 0;
};

// The venue's answer to "what do I have open, and what do I own?".
struct VenueSnapshot {
  // False means we could not ask -- no session, a timeout, an unreadable drop
  // copy. It is deliberately distinct from "asked and got nothing back":
  // an empty snapshot says there are no open orders, an unavailable one says
  // nothing at all, and treating the second as the first is how an orphaned
  // order gets missed.
  bool available = false;
  Nanos as_of_ns = 0;
  std::vector<VenueOrder> open_orders;
  std::vector<VenuePosition> positions;
};

// Where a snapshot comes from. A real implementation issues an order-status
// request over the order-entry session or reads the venue's drop copy; the
// file-backed one below lets an operator supply what the venue reported when
// there is no session to ask over.
class VenueStateSource {
 public:
  virtual ~VenueStateSource() = default;
  // Returns false and sets `error` if the venue could not be queried. `out` is
  // left with available = false in that case.
  virtual bool fetch(VenueSnapshot& out, std::string& error) = 0;
  virtual const char* name() const = 0;
};

// Reads a snapshot from a text file. The format is deliberately one an operator
// can produce by hand from a venue's web terminal at 06:00:
//
//   # comments and blank lines are ignored
//   ORDER,<cl_ord_id>,<venue_order_id>,<symbol>,<BUY|SELL>,<price>,<qty>,<filled>,<leaves>
//   POSITION,<symbol>,<signed position>
//
// `symbol` is the instrument name, resolved through the registry. An unknown
// name is an error rather than a skipped line: silently dropping the one order
// whose symbol you typo'd defeats the entire purpose of reconciling.
class FileVenueStateSource : public VenueStateSource {
 public:
  FileVenueStateSource(std::string path, const InstrumentRegistry* instruments)
      : path_(std::move(path)), instruments_(instruments) {}

  bool fetch(VenueSnapshot& out, std::string& error) override;
  const char* name() const override { return "file"; }

 private:
  std::string path_;
  const InstrumentRegistry* instruments_;
};

// The ways our view and the venue's can disagree, ordered by how much they
// should worry you.
enum class BreakKind : std::uint8_t {
  // The venue is holding a live order we are not tracking. We cannot cancel
  // it, no limit accounts for it, and it can fill at any time.
  OrphanAtVenue = 0,
  // We think an order is working; the venue has no record of it resting. It
  // either never arrived or it is already done and we missed the report.
  MissingAtVenue,
  // Both sides know the order but disagree about how much is left.
  QuantityMismatch,
  // Both sides know the order but disagree about its terms. A price or side
  // mismatch means the id refers to two different orders, which is worse than
  // a quantity drift even though it is rarer.
  TermsMismatch,
  // Our recovered position for a symbol is not the venue's.
  PositionMismatch,
  kCount
};

const char* to_string(BreakKind k);

struct Discrepancy {
  BreakKind kind = BreakKind::OrphanAtVenue;
  ClOrdId cl_ord_id = 0;
  SymbolId symbol = 0;
  std::int64_t ours = 0;    // our number, in whatever unit the kind implies
  std::int64_t theirs = 0;  // the venue's
  std::string detail;
};

struct ReconciliationReport {
  bool venue_available = false;
  std::string error;  // why the venue could not be reached, if it could not

  std::size_t orders_ours = 0;    // working orders we recovered
  std::size_t orders_theirs = 0;  // orders the venue reports open
  std::size_t orders_agreed = 0;  // matched on id, terms and quantity

  std::vector<Discrepancy> discrepancies;

  // Orders both sides agree are still working. These are safe to adopt into
  // the OMS so that risk sees them; they are not breaks.
  std::vector<VenueOrder> agreed_open;

  // Clean means: we reached the venue, and every order and position agrees.
  // Note that clean does *not* mean flat -- there may be live orders, and the
  // point of `agreed_open` is that the engine can take responsibility for them.
  bool clean() const { return venue_available && discrepancies.empty(); }

  std::size_t count(BreakKind kind) const;
  std::string summary() const;
};

// Compares recovered state against a venue snapshot. Pure: it reads both sides
// and reports, and changes neither.
ReconciliationReport reconcile(const RecoveredState& ours, const VenueSnapshot& theirs);

}  // namespace hft
