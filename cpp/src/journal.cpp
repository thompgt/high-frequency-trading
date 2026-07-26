#include "hft/journal.hpp"

#include <cstring>
#include <sstream>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace hft {
namespace {

// Table-driven CRC-32, built once on first use.
const std::uint32_t* crc_table() {
  static std::uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    built = true;
  }
  return table;
}

// Pushes a record's bytes to the storage device. The distinction this makes --
// "the OS has it" versus "the disk has it" -- is the whole point of SyncPolicy.
bool fsync_file(std::FILE* f) {
  if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
  return _commit(_fileno(f)) == 0;
#else
  return ::fsync(fileno(f)) == 0;
#endif
}

std::uint32_t checksum_of(const JournalRecord& rec) {
  JournalRecord copy = rec;
  copy.crc = 0;  // the checksum cannot cover itself
  return crc32(&copy, sizeof(copy));
}

}  // namespace

std::uint32_t crc32(const void* data, std::size_t length, std::uint32_t seed) {
  const std::uint32_t* table = crc_table();
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint32_t c = seed ^ 0xFFFFFFFFu;
  for (std::size_t i = 0; i < length; ++i) {
    c = table[(c ^ bytes[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

const char* to_string(JournalEvent e) {
  switch (e) {
    case JournalEvent::SessionStart: return "session_start";
    case JournalEvent::OrderSent: return "order_sent";
    case JournalEvent::ExecReport: return "exec_report";
    case JournalEvent::Halt: return "halt";
    case JournalEvent::Resume: return "resume";
    case JournalEvent::Checkpoint: return "checkpoint";
    case JournalEvent::SessionEnd: return "session_end";
    case JournalEvent::kCount: break;
  }
  return "unknown";
}

const char* to_string(SyncPolicy p) {
  switch (p) {
    case SyncPolicy::OnWrite: return "on_write";
    case SyncPolicy::Always: return "always";
    case SyncPolicy::Interval: return "interval";
  }
  return "unknown";
}

bool parse_sync_policy(const std::string& text, SyncPolicy& out) {
  if (text == "on_write") { out = SyncPolicy::OnWrite; return true; }
  if (text == "always") { out = SyncPolicy::Always; return true; }
  if (text == "interval") { out = SyncPolicy::Interval; return true; }
  return false;
}

// ------------------------------------------------------------------ Journal

Journal::~Journal() { close(); }

bool Journal::open(const Config& config) {
  close();
  cfg_ = config;
  error_.clear();
  seq_ = 0;
  bytes_ = 0;
  since_sync_ = 0;
  write_failures_ = 0;

  if (cfg_.path.empty()) {
    error_ = "journal path is empty";
    return false;
  }
  if (cfg_.sync_interval == 0) cfg_.sync_interval = 1;

  // Does it already exist, and is it non-empty? Appending to a file whose
  // header we have not validated is how you end up with an unreadable journal.
  bool existing = false;
  JournalHeader header;
  if (cfg_.append) {
    if (std::FILE* probe = std::fopen(cfg_.path.c_str(), "rb")) {
      JournalHeader on_disk{};
      const std::size_t got = std::fread(&on_disk, 1, sizeof(on_disk), probe);
      std::fclose(probe);
      if (got == sizeof(on_disk)) {
        if (on_disk.magic != kJournalMagic) {
          error_ = "'" + cfg_.path + "' is not a journal file";
          return false;
        }
        if (on_disk.version != kJournalVersion || on_disk.record_size != sizeof(JournalRecord)) {
          error_ = "'" + cfg_.path + "' was written by a different journal format (version " +
                   std::to_string(on_disk.version) + ", record size " +
                   std::to_string(on_disk.record_size) + ")";
          return false;
        }
        existing = true;
        header = on_disk;
      } else if (got != 0) {
        error_ = "'" + cfg_.path + "' is too short to contain a journal header";
        return false;
      }
    }
  }

  file_ = std::fopen(cfg_.path.c_str(), (cfg_.append && existing) ? "ab" : "wb");
  if (file_ == nullptr) {
    error_ = "could not open journal '" + cfg_.path + "' for writing";
    return false;
  }

  if (existing) {
    // Continue the sequence where the previous session left off, so a gap in
    // sequence numbers always means a lost record and never a restart.
    const RecoveredState prior = recover_from_journal(cfg_.path);
    seq_ = prior.records_read;
    bytes_ = sizeof(JournalHeader) + prior.records_read * sizeof(JournalRecord);
    (void)header;
  } else {
    JournalHeader fresh;
    fresh.created_ts_ns = 0;
    if (std::fwrite(&fresh, sizeof(fresh), 1, file_) != 1) {
      error_ = "could not write journal header to '" + cfg_.path + "'";
      std::fclose(file_);
      file_ = nullptr;
      return false;
    }
    bytes_ = sizeof(fresh);
    std::fflush(file_);
  }
  return true;
}

void Journal::close() {
  if (file_ == nullptr) return;
  fsync_file(file_);
  std::fclose(file_);
  file_ = nullptr;
}

bool Journal::append(JournalRecord& rec) {
  if (file_ == nullptr) return false;

  rec.size = sizeof(JournalRecord);
  rec.seq = ++seq_;
  rec.crc = checksum_of(rec);

  if (std::fwrite(&rec, sizeof(rec), 1, file_) != 1) {
    ++write_failures_;
    error_ = "journal write failed";
    return false;
  }
  bytes_ += sizeof(rec);
  ++since_sync_;

  switch (cfg_.sync) {
    case SyncPolicy::OnWrite:
      // Hand it to the OS. Survives the process dying; does not survive the
      // machine dying.
      if (std::fflush(file_) != 0) {
        ++write_failures_;
        error_ = "journal flush failed";
        return false;
      }
      break;
    case SyncPolicy::Always:
      if (!fsync_file(file_)) {
        ++write_failures_;
        error_ = "journal fsync failed";
        return false;
      }
      since_sync_ = 0;
      break;
    case SyncPolicy::Interval:
      if (since_sync_ >= cfg_.sync_interval) {
        if (!fsync_file(file_)) {
          ++write_failures_;
          error_ = "journal fsync failed";
          return false;
        }
        since_sync_ = 0;
      }
      break;
  }
  return true;
}

bool Journal::record_session_start(Nanos ts_ns) {
  JournalRecord rec;
  rec.type = static_cast<std::uint8_t>(JournalEvent::SessionStart);
  rec.ts_ns = ts_ns;
  return append(rec);
}

bool Journal::record_session_end(Nanos ts_ns) {
  JournalRecord rec;
  rec.type = static_cast<std::uint8_t>(JournalEvent::SessionEnd);
  rec.ts_ns = ts_ns;
  const bool ok = append(rec);
  // A clean shutdown is worth paying for: this is the record that tells the
  // next start it has nothing to reconcile.
  return ok && sync();
}

bool Journal::record_order_sent(ClOrdId cl_ord_id, const Order& order, Nanos ts_ns) {
  JournalRecord rec;
  rec.type = static_cast<std::uint8_t>(JournalEvent::OrderSent);
  rec.ts_ns = ts_ns;
  rec.cl_ord_id = cl_ord_id;
  rec.symbol = order.symbol;
  rec.side = static_cast<std::uint8_t>(order.side);
  rec.order_type = static_cast<std::uint8_t>(order.type);
  rec.price = order.price;
  rec.quantity = order.quantity;
  return append(rec);
}

bool Journal::record_exec_report(const ExecutionReport& report) {
  JournalRecord rec;
  rec.type = static_cast<std::uint8_t>(JournalEvent::ExecReport);
  rec.ts_ns = report.ts_ns;
  rec.cl_ord_id = report.cl_ord_id;
  rec.venue_order_id = report.venue_order_id;
  rec.symbol = report.symbol;
  rec.side = static_cast<std::uint8_t>(report.side);
  rec.exec_type = static_cast<std::uint8_t>(report.type);
  rec.price = report.price;
  rec.quantity = report.quantity;
  rec.fee = report.fee;
  return append(rec);
}

bool Journal::record_halt(std::uint8_t reason, Nanos ts_ns) {
  JournalRecord rec;
  rec.type = static_cast<std::uint8_t>(JournalEvent::Halt);
  rec.ts_ns = ts_ns;
  rec.reason = reason;
  const bool ok = append(rec);
  return ok && sync();  // never lose the record of why we stopped
}

bool Journal::record_resume(Nanos ts_ns) {
  JournalRecord rec;
  rec.type = static_cast<std::uint8_t>(JournalEvent::Resume);
  rec.ts_ns = ts_ns;
  return append(rec);
}

bool Journal::record_checkpoint(SymbolId symbol, std::int64_t position, double realized_pnl,
                                double fees, Nanos ts_ns) {
  JournalRecord rec;
  rec.type = static_cast<std::uint8_t>(JournalEvent::Checkpoint);
  rec.ts_ns = ts_ns;
  rec.symbol = symbol;
  rec.position = position;
  rec.realized_pnl = realized_pnl;
  rec.fee = fees;
  return append(rec);
}

bool Journal::sync() {
  if (file_ == nullptr) return false;
  if (!fsync_file(file_)) {
    ++write_failures_;
    error_ = "journal fsync failed";
    return false;
  }
  since_sync_ = 0;
  return true;
}

// ----------------------------------------------------------------- recovery

RecoveredState recover_from_journal(const std::string& path) {
  RecoveredState state;

  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    // No file means no previous session. That is a first run, not a failure.
    state.ok = true;
    return state;
  }

  JournalHeader header{};
  const std::size_t header_bytes = std::fread(&header, 1, sizeof(header), f);
  if (header_bytes == 0) {
    std::fclose(f);
    state.ok = true;  // created but never written to
    return state;
  }
  if (header_bytes != sizeof(header) || header.magic != kJournalMagic) {
    std::fclose(f);
    state.error = "'" + path + "' is not a journal file";
    return state;
  }
  if (header.version != kJournalVersion || header.record_size != sizeof(JournalRecord)) {
    std::fclose(f);
    state.error = "'" + path + "' has journal format version " + std::to_string(header.version) +
                  ", this build reads version " + std::to_string(kJournalVersion);
    return state;
  }

  // Replay into a real OrderManager so recovery uses exactly the same state
  // machine as the live path. A second, "simpler" replay implementation would
  // be one more thing to keep in step, and it would be the copy nobody tests.
  OrderManager::Config oms_cfg;
  oms_cfg.max_open_orders = 1u << 20;
  oms_cfg.retired_history = 1u << 20;
  OrderManager oms(oms_cfg);

  std::uint64_t expected_seq = 0;
  for (;;) {
    JournalRecord rec{};
    const std::size_t got = std::fread(&rec, 1, sizeof(rec), f);
    if (got == 0) break;
    if (got != sizeof(rec)) {
      // A partial record at the end of the file is the write that was in
      // flight when we died. Expected, not corruption.
      state.truncated_bytes += got;
      break;
    }
    if (rec.size != sizeof(JournalRecord) || rec.crc != checksum_of(rec)) {
      // Peek ahead: a bad record at the very end is a torn tail; a bad record
      // with valid data after it is real corruption and must be shouted about.
      const long here = std::ftell(f);
      std::fseek(f, 0, SEEK_END);
      const long end = std::ftell(f);
      if (here == end) {
        state.truncated_bytes += sizeof(rec);
      } else {
        ++state.corrupt_records;
        std::fseek(f, here, SEEK_SET);
        continue;
      }
      break;
    }

    if (expected_seq != 0 && rec.seq != expected_seq + 1) ++state.sequence_gaps;
    expected_seq = rec.seq;
    ++state.records_read;
    state.last_ts_ns = rec.ts_ns;
    state.clean_shutdown = false;

    switch (static_cast<JournalEvent>(rec.type)) {
      case JournalEvent::SessionStart:
        break;

      case JournalEvent::OrderSent: {
        Order order{};
        order.symbol = rec.symbol;
        order.side = static_cast<Side>(rec.side);
        order.type = static_cast<OrderType>(rec.order_type);
        order.price = rec.price;
        order.quantity = rec.quantity;
        // Use the id the journal recorded rather than letting the OMS assign a
        // fresh one. Re-deriving ids only works if the previous run's id
        // sequence can be reproduced exactly, which it cannot across a
        // restart -- and a mismatch makes every subsequent execution report
        // unmatchable, so the whole session replays as unacknowledged orders.
        oms.create_with_id(rec.cl_ord_id, order, rec.ts_ns);
        if (rec.cl_ord_id > state.next_cl_ord_id) state.next_cl_ord_id = rec.cl_ord_id;
        break;
      }

      case JournalEvent::ExecReport: {
        ExecutionReport report{};
        report.cl_ord_id = rec.cl_ord_id;
        report.venue_order_id = rec.venue_order_id;
        report.symbol = rec.symbol;
        report.side = static_cast<Side>(rec.side);
        report.type = static_cast<ExecType>(rec.exec_type);
        report.price = rec.price;
        report.quantity = rec.quantity;
        report.fee = rec.fee;
        report.ts_ns = rec.ts_ns;
        oms.apply(report);

        if (report.type == ExecType::Fill && report.quantity > 0) {
          if (report.symbol >= state.positions.size()) {
            state.positions.resize(static_cast<std::size_t>(report.symbol) + 1, 0);
          }
          state.positions[report.symbol] +=
              (report.side == Side::Buy ? report.quantity : -report.quantity);
          state.fees_paid += report.fee;
        }
        break;
      }

      case JournalEvent::Halt:
        state.halted = true;
        state.halt_reason = rec.reason;
        break;

      case JournalEvent::Resume:
        state.halted = false;
        state.halt_reason = 0;
        break;

      case JournalEvent::Checkpoint:
        // A checkpoint is authoritative for PnL, which cannot be rebuilt from
        // fills alone without re-deriving the venue's average-cost accounting.
        state.realized_pnl = rec.realized_pnl;
        state.fees_paid = rec.fee;
        if (rec.symbol >= state.positions.size()) {
          state.positions.resize(static_cast<std::size_t>(rec.symbol) + 1, 0);
        }
        state.positions[rec.symbol] = rec.position;
        break;

      case JournalEvent::SessionEnd:
        state.clean_shutdown = true;
        break;

      case JournalEvent::kCount:
        break;
    }
  }
  std::fclose(f);

  for (const ClOrdId id : oms.open_orders()) {
    if (const OrderRecord* rec = oms.find(id)) state.open_orders.push_back(*rec);
  }

  state.ok = true;
  return state;
}

std::string RecoveredState::summary() const {
  std::ostringstream os;
  if (!ok) {
    os << "journal recovery FAILED: " << error << "\n";
    return os.str();
  }
  os << "records replayed    : " << records_read << "\n";
  os << "clean shutdown      : " << (clean_shutdown ? "yes" : "NO -- previous session did not "
                                                             "finish cleanly")
     << "\n";
  if (truncated_bytes > 0) {
    os << "torn tail           : " << truncated_bytes
       << " byte(s) discarded (a write was in flight when the process died)\n";
  }
  if (corrupt_records > 0) {
    os << "CORRUPT records     : " << corrupt_records
       << " -- recovered state is incomplete, reconcile with the venue\n";
  }
  if (sequence_gaps > 0) {
    os << "sequence gaps       : " << sequence_gaps << " -- records were lost\n";
  }
  os << "realized pnl        : " << realized_pnl << "\n";
  os << "fees paid           : " << fees_paid << "\n";
  for (std::size_t i = 0; i < positions.size(); ++i) {
    if (positions[i] != 0) os << "position symbol " << i << "   : " << positions[i] << "\n";
  }
  if (halted) os << "HALTED at shutdown  : reason code " << int(halt_reason) << "\n";
  if (!open_orders.empty()) {
    os << "OPEN ORDERS         : " << open_orders.size()
       << " -- these may still be live at the venue; reconcile before quoting\n";
    for (const auto& o : open_orders) {
      os << "  cl_ord_id=" << o.cl_ord_id << " symbol=" << o.symbol << " " << to_string(o.side)
         << " " << o.leaves << " @ " << price_to_double(o.price) << " state=" << to_string(o.state)
         << "\n";
    }
  }
  return os.str();
}

}  // namespace hft
