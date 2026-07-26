// Append-only event journal, and recovery from it.
//
// The problem
// -----------
// Everything the engine knows -- positions, working orders, realized PnL --
// lives in memory. Kill the process and it is all gone. That is survivable in a
// backtest and unacceptable in production: after a crash the *venue* still
// remembers your position and your resting orders, and if you restart without
// knowing what they are you will trade on top of them. The first job on restart
// is not to trade, it is to find out what you already own.
//
// So every decision and every venue response is appended here before it is
// acted on, and a restart replays the file to rebuild what was true.
//
// What "durable" means here
// -------------------------
// There are two failure modes and they need different treatment, so the policy
// is explicit rather than implied:
//
//   * **Process crash** (segfault, SIGKILL, an assertion). Data that has
//     reached the OS survives, because the kernel owns the page cache and the
//     kernel did not die. A plain write is enough. This is the common case.
//   * **Power loss / kernel panic.** Only data the storage device has actually
//     committed survives, which means fsync. That costs on the order of a
//     millisecond, so doing it per record would cap the engine at ~1000
//     orders/second -- which is why it is a configurable policy and not a
//     default.
//
// SyncPolicy::OnWrite is the honest default: crash-safe, not power-safe, and
// fast. Anyone who needs power-safety can pay for it with SyncPolicy::Always
// and should measure what it costs them.
//
// Format
// ------
// A file header, then fixed-size 96-byte records, each with its own CRC32 over
// its contents and a monotonically increasing sequence number. Fixed-size
// records waste some space against a packed variable-length encoding; they buy
// a reader that cannot get out of step with the writer, which is the property
// that matters when the file you are reading was truncated mid-write by the
// crash you are recovering from.
//
// A torn tail is expected, not exceptional: the reader stops at the first
// record that fails validation and reports how many bytes it discarded.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "hft/oms.hpp"
#include "hft/types.hpp"

namespace hft {

// CRC-32 (IEEE 802.3, the zlib polynomial). Exposed because the tests check it
// against known vectors -- a silently wrong checksum is worse than none.
std::uint32_t crc32(const void* data, std::size_t length, std::uint32_t seed = 0);

inline constexpr std::uint64_t kJournalMagic = 0x314C4E524A544648ULL;  // "HFTJRNL1"
inline constexpr std::uint32_t kJournalVersion = 1;

enum class JournalEvent : std::uint8_t {
  SessionStart = 0,  // engine came up
  OrderSent,         // we decided to trade, before the order leaves
  ExecReport,        // what the venue told us
  Halt,              // kill switch engaged
  Resume,            // kill switch released
  Checkpoint,        // a position/PnL snapshot, to bound replay
  SessionEnd,        // clean shutdown -- its absence is itself information
  kCount
};

const char* to_string(JournalEvent e);

// One journal entry. Flat rather than a union: every event writes the same
// 96-byte shape, so validating a record never depends on trusting its own type
// tag -- which is exactly the field you cannot trust in a corrupt record.
struct JournalRecord {
  std::uint32_t crc = 0;   // over the record with this field zeroed
  std::uint32_t size = 0;  // always sizeof(JournalRecord); guards format drift
  std::uint64_t seq = 0;
  Nanos ts_ns = 0;

  std::uint8_t type = 0;        // JournalEvent
  std::uint8_t side = 0;        // Side
  std::uint8_t order_type = 0;  // OrderType
  std::uint8_t exec_type = 0;   // ExecType
  std::uint8_t reason = 0;      // HaltReason, for Halt records
  std::uint8_t pad_[3] = {0, 0, 0};

  ClOrdId cl_ord_id = 0;
  OrderId venue_order_id = 0;
  SymbolId symbol = 0;
  std::uint32_t pad2_ = 0;

  Price price = 0;
  Quantity quantity = 0;
  std::int64_t position = 0;  // Checkpoint only
  double fee = 0.0;
  double realized_pnl = 0.0;  // Checkpoint only
};

static_assert(sizeof(JournalRecord) == 96, "journal record size is part of the on-disk format");

struct JournalHeader {
  std::uint64_t magic = kJournalMagic;
  std::uint32_t version = kJournalVersion;
  std::uint32_t record_size = sizeof(JournalRecord);
  Nanos created_ts_ns = 0;
  std::uint64_t reserved = 0;
};

enum class SyncPolicy : std::uint8_t {
  // Hand each record to the OS immediately. Survives a process crash; does not
  // survive power loss. Fast. The right default for most engines.
  OnWrite = 0,
  // fsync after every record. Survives power loss, costs ~1ms per record.
  Always,
  // fsync every `sync_interval` records: bounded loss, amortised cost.
  Interval,
};

const char* to_string(SyncPolicy p);
bool parse_sync_policy(const std::string& text, SyncPolicy& out);

class Journal {
 public:
  struct Config {
    std::string path;
    SyncPolicy sync = SyncPolicy::OnWrite;
    std::uint64_t sync_interval = 256;  // records, for SyncPolicy::Interval
    // Append to an existing file rather than truncating it. On by default:
    // silently destroying yesterday's record of what you traded is not a
    // behaviour a journal should ever have.
    bool append = true;
  };

  Journal() = default;
  ~Journal();

  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  // Opens (creating if needed) the journal. Returns false and sets error() if
  // the file cannot be opened or its header does not match this build's format
  // -- appending records in one layout to a file written in another would
  // corrupt exactly the data you need after a crash.
  bool open(const Config& config);
  void close();
  bool is_open() const { return file_ != nullptr; }

  // --- append paths --------------------------------------------------------
  // Each returns false if the write failed. A journal that has started failing
  // must be treated as a reason to stop trading, not a warning to log.
  bool record_session_start(Nanos ts_ns);
  bool record_session_end(Nanos ts_ns);
  bool record_order_sent(ClOrdId cl_ord_id, const Order& order, Nanos ts_ns);
  bool record_exec_report(const ExecutionReport& report);
  bool record_halt(std::uint8_t reason, Nanos ts_ns);
  bool record_resume(Nanos ts_ns);
  bool record_checkpoint(SymbolId symbol, std::int64_t position, double realized_pnl,
                         double fees, Nanos ts_ns);

  // Forces everything to the storage device regardless of policy. Called on
  // clean shutdown.
  bool sync();

  std::uint64_t records_written() const { return seq_; }
  std::uint64_t bytes_written() const { return bytes_; }
  std::uint64_t write_failures() const { return write_failures_; }
  const std::string& error() const { return error_; }
  const Config& config() const { return cfg_; }

 private:
  bool append(JournalRecord& rec);

  Config cfg_;
  std::FILE* file_ = nullptr;
  std::uint64_t seq_ = 0;
  std::uint64_t bytes_ = 0;
  std::uint64_t since_sync_ = 0;
  std::uint64_t write_failures_ = 0;
  std::string error_;
};

// --------------------------------------------------------------- recovery

// What a restart needs to know before it is allowed to trade.
struct RecoveredState {
  bool ok = false;
  std::string error;

  std::uint64_t records_read = 0;
  // Bytes at the end of the file that did not form a valid record. Non-zero is
  // normal after a crash -- it is the write that was in flight when the process
  // died -- and is reported rather than hidden.
  std::uint64_t truncated_bytes = 0;
  // A record whose CRC failed *before* the end of the file. This is not a torn
  // tail, it is corruption, and it means the replay below is incomplete.
  std::uint64_t corrupt_records = 0;
  // Records whose sequence number did not follow the previous one.
  std::uint64_t sequence_gaps = 0;

  // True when the file ends with a SessionEnd record: the engine shut down
  // cleanly and nothing here needs reconciling with the venue.
  bool clean_shutdown = false;

  // Position per symbol, rebuilt from the fills the venue confirmed.
  std::vector<std::int64_t> positions;
  double realized_pnl = 0.0;
  double fees_paid = 0.0;

  // Orders that were sent and never reached a terminal state. These are the
  // dangerous ones: they may be resting at the venue right now. A restart must
  // query the venue about each before quoting again.
  std::vector<OrderRecord> open_orders;

  bool halted = false;
  std::uint8_t halt_reason = 0;
  Nanos last_ts_ns = 0;

  std::int64_t position(SymbolId symbol) const {
    return symbol < positions.size() ? positions[symbol] : 0;
  }
  std::string summary() const;
};

// Replays `path` and rebuilds state. A missing file is not an error -- it means
// a first run -- and comes back with ok = true and nothing recovered.
RecoveredState recover_from_journal(const std::string& path);

}  // namespace hft
