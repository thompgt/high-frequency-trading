// Journal and recovery tests.
//
// The interesting cases are all failure cases: a file truncated mid-write, a
// record whose bits flipped, a session that never wrote its end marker. Those
// are the states an actual crash leaves behind, so they are what recovery has
// to handle correctly.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "hft/journal.hpp"
#include "test_harness.hpp"

using namespace hft;

namespace {

// Scratch files live under build/, matching where the rest of the suite writes.
std::string scratch(const char* name) { return std::string("build/") + name; }

void remove_file(const std::string& path) { std::remove(path.c_str()); }

Order journal_order(SymbolId sym, Side side, Price px, Quantity qty) {
  Order o{};
  o.symbol = sym;
  o.side = side;
  o.type = OrderType::Limit;
  o.price = px;
  o.quantity = qty;
  return o;
}

Order make_journal_order() { return journal_order(0, Side::Buy, 10000, 10); }

ExecutionReport journal_report(ClOrdId id, ExecType type, SymbolId sym, Side side, Price px,
                               Quantity qty, double fee = 0.0) {
  ExecutionReport r{};
  r.cl_ord_id = id;
  r.type = type;
  r.symbol = sym;
  r.side = side;
  r.price = px;
  r.quantity = qty;
  r.fee = fee;
  r.ts_ns = 1000 + static_cast<Nanos>(id);
  return r;
}

// Writes a complete, well-formed session: two orders, both filled.
void write_simple_session(const std::string& path, bool clean_end) {
  Journal j;
  Journal::Config cfg;
  cfg.path = path;
  cfg.append = false;
  CHECK(j.open(cfg));
  CHECK(j.record_session_start(1));

  CHECK(j.record_order_sent(1, journal_order(0, Side::Buy, 10000, 100), 10));
  CHECK(j.record_exec_report(journal_report(1, ExecType::Acked, 0, Side::Buy, 0, 0)));
  CHECK(j.record_exec_report(journal_report(1, ExecType::Fill, 0, Side::Buy, 10000, 100, 0.5)));

  CHECK(j.record_order_sent(2, journal_order(0, Side::Sell, 10100, 40), 20));
  CHECK(j.record_exec_report(journal_report(2, ExecType::Acked, 0, Side::Sell, 0, 0)));
  CHECK(j.record_exec_report(journal_report(2, ExecType::Fill, 0, Side::Sell, 10100, 40, 0.2)));

  if (clean_end) CHECK(j.record_session_end(99));
  j.close();
}

// Chops `bytes` off the end of a file, standing in for the process dying
// mid-write.
void truncate_file(const std::string& path, std::size_t bytes) {
  std::ifstream in(path, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  CHECK(data.size() > bytes);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size() - bytes));
}

// Flips a byte at an absolute offset.
void corrupt_byte(const std::string& path, std::size_t offset) {
  std::ifstream in(path, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  CHECK(data.size() > offset);
  data[offset] = static_cast<char>(data[offset] ^ 0x5A);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

}  // namespace

// ==================================================================== crc32

TEST(journal_crc32_matches_known_vectors) {
  // Standard CRC-32/ISO-HDLC check values. If these drift, every journal
  // written by this build becomes unreadable by every other one.
  CHECK_EQ(crc32("", 0), std::uint32_t(0x00000000));
  CHECK_EQ(crc32("a", 1), std::uint32_t(0xE8B7BE43));
  CHECK_EQ(crc32("123456789", 9), std::uint32_t(0xCBF43926));
  CHECK_EQ(crc32("The quick brown fox jumps over the lazy dog", 43),
           std::uint32_t(0x414FA339));
}

TEST(journal_crc32_detects_single_bit_flips) {
  const std::string base = "an order for 100 lots at 100.00";
  const std::uint32_t good = crc32(base.data(), base.size());
  for (std::size_t i = 0; i < base.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string bad = base;
      bad[i] = static_cast<char>(bad[i] ^ (1 << bit));
      CHECK_NE(crc32(bad.data(), bad.size()), good);
    }
  }
}

// =============================================================== round trip

TEST(journal_writes_and_recovers_a_clean_session) {
  const std::string path = scratch("journal_clean.jrn");
  remove_file(path);
  write_simple_session(path, /*clean_end=*/true);

  const RecoveredState state = recover_from_journal(path);
  CHECK(state.ok);
  CHECK_EQ(state.error, std::string());
  CHECK(state.clean_shutdown);
  CHECK_EQ(state.truncated_bytes, std::uint64_t(0));
  CHECK_EQ(state.corrupt_records, std::uint64_t(0));
  CHECK_EQ(state.sequence_gaps, std::uint64_t(0));

  // 100 bought, 40 sold.
  CHECK_EQ(state.position(0), std::int64_t(60));
  CHECK_NEAR(state.fees_paid, 0.7, 1e-9);
  // Both orders reached a terminal state, so nothing needs reconciling.
  CHECK_EQ(state.open_orders.size(), std::size_t(0));
  CHECK_FALSE(state.halted);
}

TEST(journal_recovery_of_a_missing_file_is_a_first_run_not_an_error) {
  const RecoveredState state = recover_from_journal(scratch("journal_does_not_exist.jrn"));
  CHECK(state.ok);
  CHECK_EQ(state.records_read, std::uint64_t(0));
  CHECK_EQ(state.open_orders.size(), std::size_t(0));
}

TEST(journal_reports_an_unclean_shutdown) {
  const std::string path = scratch("journal_unclean.jrn");
  remove_file(path);
  write_simple_session(path, /*clean_end=*/false);

  const RecoveredState state = recover_from_journal(path);
  CHECK(state.ok);
  // No SessionEnd record: the previous run did not get to shut down.
  CHECK_FALSE(state.clean_shutdown);
  CHECK_EQ(state.position(0), std::int64_t(60));
}

// ============================================================ open orders

TEST(journal_recovers_orders_that_never_reached_a_terminal_state) {
  // The dangerous case: we sent an order, the process died before the venue
  // answered, and that order may be resting at the venue right now.
  const std::string path = scratch("journal_open.jrn");
  remove_file(path);
  {
    Journal j;
    Journal::Config cfg;
    cfg.path = path;
    cfg.append = false;
    CHECK(j.open(cfg));
    j.record_session_start(1);
    j.record_order_sent(1, journal_order(0, Side::Buy, 10000, 100), 10);
    j.record_exec_report(journal_report(1, ExecType::Acked, 0, Side::Buy, 0, 0));
    j.record_exec_report(journal_report(1, ExecType::Fill, 0, Side::Buy, 10000, 30));
    // Order 2 is sent and never heard from again.
    j.record_order_sent(2, journal_order(0, Side::Sell, 10100, 55), 20);
    j.close();
  }

  const RecoveredState state = recover_from_journal(path);
  CHECK(state.ok);
  CHECK_FALSE(state.clean_shutdown);
  CHECK_EQ(state.position(0), std::int64_t(30));

  CHECK_EQ(state.open_orders.size(), std::size_t(2));
  // Order 1 is partly filled with 70 still working.
  CHECK_EQ(state.open_orders[0].cl_ord_id, ClOrdId(1));
  CHECK_EQ(state.open_orders[0].leaves, Quantity(70));
  CHECK_EQ(int(state.open_orders[0].state), int(OrderState::PartiallyFilled));
  // Order 2 was never acknowledged: we do not know whether it exists.
  CHECK_EQ(state.open_orders[1].cl_ord_id, ClOrdId(2));
  CHECK_EQ(state.open_orders[1].leaves, Quantity(55));
  CHECK_EQ(int(state.open_orders[1].state), int(OrderState::PendingNew));

  const std::string text = state.summary();
  CHECK(text.find("OPEN ORDERS") != std::string::npos);
  CHECK(text.find("reconcile before quoting") != std::string::npos);
}

// ================================================================ halts

TEST(journal_recovers_the_halted_state) {
  const std::string path = scratch("journal_halt.jrn");
  remove_file(path);
  {
    Journal j;
    Journal::Config cfg;
    cfg.path = path;
    cfg.append = false;
    CHECK(j.open(cfg));
    j.record_session_start(1);
    j.record_halt(2, 50);
    j.close();
  }
  RecoveredState state = recover_from_journal(path);
  CHECK(state.halted);
  CHECK_EQ(int(state.halt_reason), 2);
  CHECK(state.summary().find("HALTED") != std::string::npos);

  // A resume clears it, so a restart does not inherit a halt that was lifted.
  {
    Journal j;
    Journal::Config cfg;
    cfg.path = path;
    cfg.append = true;
    CHECK(j.open(cfg));
    j.record_resume(60);
    j.close();
  }
  state = recover_from_journal(path);
  CHECK_FALSE(state.halted);
}

// ========================================================= crash tolerance

TEST(journal_tolerates_a_torn_tail) {
  // The process died partway through writing a record. Everything before it is
  // still good and must still be recovered.
  const std::string path = scratch("journal_torn.jrn");
  remove_file(path);
  write_simple_session(path, /*clean_end=*/true);
  truncate_file(path, 40);  // less than one whole record

  const RecoveredState state = recover_from_journal(path);
  CHECK(state.ok);
  CHECK_EQ(state.truncated_bytes, std::uint64_t(sizeof(JournalRecord) - 40));
  CHECK_EQ(state.corrupt_records, std::uint64_t(0));
  CHECK_FALSE(state.clean_shutdown);  // the SessionEnd record is what was lost
  // The fills that were written before the crash survive.
  CHECK_EQ(state.position(0), std::int64_t(60));
  CHECK(state.summary().find("torn tail") != std::string::npos);
}

TEST(journal_tolerates_losing_several_whole_records) {
  const std::string path = scratch("journal_torn2.jrn");
  remove_file(path);
  write_simple_session(path, /*clean_end=*/true);
  truncate_file(path, sizeof(JournalRecord) * 3);

  const RecoveredState state = recover_from_journal(path);
  CHECK(state.ok);
  // The session wrote 8 records; three went with the tail.
  CHECK_EQ(state.records_read, std::uint64_t(5));
  // The buy filled; the sell's records went with the tail, so it is still open.
  CHECK_EQ(state.position(0), std::int64_t(100));
  CHECK_EQ(state.open_orders.size(), std::size_t(1));
}

TEST(journal_reports_corruption_in_the_middle_of_the_file) {
  // A bad record with good data after it is not a torn tail. Recovery must say
  // so loudly: the state it rebuilt is missing something.
  const std::string path = scratch("journal_corrupt.jrn");
  remove_file(path);
  write_simple_session(path, /*clean_end=*/true);
  // Land in the middle of the third record's payload.
  corrupt_byte(path, sizeof(JournalHeader) + sizeof(JournalRecord) * 2 + 40);

  const RecoveredState state = recover_from_journal(path);
  CHECK(state.ok);
  CHECK_EQ(state.corrupt_records, std::uint64_t(1));
  // Reading continues past the damage rather than throwing the rest away.
  CHECK(state.records_read > 0);
  CHECK(state.summary().find("CORRUPT") != std::string::npos);
  CHECK(state.summary().find("reconcile with the venue") != std::string::npos);
}

TEST(journal_rejects_a_file_that_is_not_a_journal) {
  const std::string path = scratch("journal_not_a_journal.jrn");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "this is a text file, not a journal, and it is long enough to have a header";
  }
  const RecoveredState state = recover_from_journal(path);
  CHECK_FALSE(state.ok);
  CHECK(state.error.find("not a journal") != std::string::npos);

  // Appending to it must also be refused, rather than producing a file that is
  // half text and half records.
  Journal j;
  Journal::Config cfg;
  cfg.path = path;
  cfg.append = true;
  CHECK_FALSE(j.open(cfg));
  CHECK(j.error().find("not a journal") != std::string::npos);
}

// ================================================================ appending

TEST(journal_appends_across_sessions_without_restarting_the_sequence) {
  // Sequence numbers must be continuous across restarts, otherwise a gap
  // cannot be distinguished from a normal restart -- and a gap means data loss.
  const std::string path = scratch("journal_append.jrn");
  remove_file(path);
  write_simple_session(path, /*clean_end=*/true);
  const std::uint64_t first_session_records = recover_from_journal(path).records_read;

  {
    Journal j;
    Journal::Config cfg;
    cfg.path = path;
    cfg.append = true;
    CHECK(j.open(cfg));
    CHECK(j.record_session_start(200));
    CHECK(j.record_order_sent(3, journal_order(1, Side::Buy, 5000, 7), 210));
    CHECK(j.record_exec_report(journal_report(3, ExecType::Acked, 1, Side::Buy, 0, 0)));
    CHECK(j.record_exec_report(journal_report(3, ExecType::Fill, 1, Side::Buy, 5000, 7)));
    CHECK(j.record_session_end(220));
    j.close();
  }

  const RecoveredState state = recover_from_journal(path);
  CHECK(state.ok);
  CHECK_EQ(state.records_read, first_session_records + 5);
  CHECK_EQ(state.sequence_gaps, std::uint64_t(0));
  CHECK(state.clean_shutdown);
  // Both sessions' fills are reflected.
  CHECK_EQ(state.position(0), std::int64_t(60));
  CHECK_EQ(state.position(1), std::int64_t(7));
}

TEST(journal_can_truncate_when_asked) {
  const std::string path = scratch("journal_truncate.jrn");
  remove_file(path);
  write_simple_session(path, true);
  {
    Journal j;
    Journal::Config cfg;
    cfg.path = path;
    cfg.append = false;  // start a fresh file
    CHECK(j.open(cfg));
    CHECK(j.record_session_start(1));
    j.close();
  }
  const RecoveredState state = recover_from_journal(path);
  CHECK_EQ(state.records_read, std::uint64_t(1));
  CHECK_EQ(state.position(0), std::int64_t(0));
}

// ============================================================= checkpoints

TEST(journal_checkpoint_is_authoritative_for_pnl) {
  const std::string path = scratch("journal_checkpoint.jrn");
  remove_file(path);
  {
    Journal j;
    Journal::Config cfg;
    cfg.path = path;
    cfg.append = false;
    CHECK(j.open(cfg));
    j.record_session_start(1);
    j.record_checkpoint(0, 250, 1234.56, 7.89, 50);
    j.record_session_end(60);
    j.close();
  }
  const RecoveredState state = recover_from_journal(path);
  CHECK_EQ(state.position(0), std::int64_t(250));
  CHECK_NEAR(state.realized_pnl, 1234.56, 1e-9);
  CHECK_NEAR(state.fees_paid, 7.89, 1e-9);
}

// ================================================================= policy

TEST(journal_sync_policy_round_trips) {
  SyncPolicy p;
  CHECK(parse_sync_policy("on_write", p));
  CHECK_EQ(int(p), int(SyncPolicy::OnWrite));
  CHECK(parse_sync_policy("always", p));
  CHECK_EQ(int(p), int(SyncPolicy::Always));
  CHECK(parse_sync_policy("interval", p));
  CHECK_EQ(int(p), int(SyncPolicy::Interval));
  CHECK_FALSE(parse_sync_policy("sometimes", p));

  CHECK_EQ(std::string(to_string(SyncPolicy::OnWrite)), std::string("on_write"));
  CHECK_EQ(std::string(to_string(SyncPolicy::Always)), std::string("always"));
}

TEST(journal_every_sync_policy_produces_a_readable_file) {
  const SyncPolicy policies[] = {SyncPolicy::OnWrite, SyncPolicy::Always, SyncPolicy::Interval};
  int index = 0;
  for (const SyncPolicy policy : policies) {
    const std::string path = scratch(("journal_policy_" + std::to_string(index++) + ".jrn").c_str());
    remove_file(path);
    Journal j;
    Journal::Config cfg;
    cfg.path = path;
    cfg.append = false;
    cfg.sync = policy;
    cfg.sync_interval = 2;
    CHECK(j.open(cfg));
    for (int i = 0; i < 10; ++i) {
      CHECK(j.record_order_sent(static_cast<ClOrdId>(i + 1),
                                journal_order(0, Side::Buy, 10000, 1), i));
    }
    CHECK(j.record_session_end(100));
    j.close();

    const RecoveredState state = recover_from_journal(path);
    CHECK(state.ok);
    CHECK_EQ(state.records_read, std::uint64_t(11));
    CHECK_EQ(state.corrupt_records, std::uint64_t(0));
    CHECK_EQ(j.write_failures(), std::uint64_t(0));
  }
}

TEST(journal_refuses_an_unwritable_path) {
  Journal j;
  Journal::Config cfg;
  cfg.path = "build/no_such_directory_here/journal.jrn";
  CHECK_FALSE(j.open(cfg));
  CHECK(!j.error().empty());
  CHECK_FALSE(j.is_open());

  // An empty path is a configuration mistake, not a silent no-op.
  Journal j2;
  Journal::Config cfg2;
  CHECK_FALSE(j2.open(cfg2));
}

TEST(journal_record_is_trivially_copyable_and_fixed_size) {
  // Both are load-bearing: the record is memcpy'd to disk and its size is part
  // of the on-disk format.
  CHECK(std::is_trivially_copyable<JournalRecord>::value);
  CHECK_EQ(sizeof(JournalRecord), std::size_t(96));
  CHECK(std::is_trivially_copyable<JournalHeader>::value);
}

// ====================================================== engine integration

#include "hft/engine.hpp"
#include "hft/feed.hpp"

TEST(engine_journals_every_order_and_the_result_replays_to_the_same_position) {
  // End-to-end: run the engine with a journal attached, then rebuild state
  // from the file alone. The recovered position has to match what the venue
  // actually holds, or recovery is worse than useless.
  const std::string path = scratch("journal_engine.jrn");
  remove_file(path);

  EngineConfig cfg;
  cfg.threaded = false;
  cfg.fast_window = 3;
  cfg.slow_window = 8;
  cfg.order_quantity = 5;
  cfg.record_curve = false;
  cfg.risk.max_orders_per_second = 1'000'000'000u;

  Journal journal;
  Journal::Config jcfg;
  jcfg.path = path;
  jcfg.append = false;
  CHECK(journal.open(jcfg));

  Engine engine(cfg);
  engine.set_journal(&journal);

  SyntheticFeed::Params fp;
  fp.total_events = 60'000;
  fp.seed = 0x1234ABCDULL;
  SyntheticFeed feed(fp);

  EngineStats stats = engine.run(feed);
  CHECK(stats.orders_sent > 0);
  CHECK_EQ(stats.journal_failures, std::uint64_t(0));
  journal.record_session_end(now_ns());
  journal.close();

  const RecoveredState state = recover_from_journal(path);
  CHECK(state.ok);
  CHECK(state.clean_shutdown);
  CHECK_EQ(state.corrupt_records, std::uint64_t(0));
  CHECK_EQ(state.sequence_gaps, std::uint64_t(0));

  // The position rebuilt from the journal matches the venue's own accounting.
  CHECK_EQ(state.position(0), engine.venue().position(0));
  // Every order the engine sent reached a terminal state, so nothing is left
  // needing reconciliation.
  CHECK_EQ(state.open_orders.size(), std::size_t(0));
}

TEST(engine_halts_when_the_journal_cannot_be_written) {
  // An engine that cannot record what it did cannot be recovered, so losing
  // the journal is a stop condition rather than a degraded mode.
  const std::string path = scratch("journal_engine_fail.jrn");
  remove_file(path);

  EngineConfig cfg;
  cfg.threaded = false;
  cfg.fast_window = 3;
  cfg.slow_window = 8;
  cfg.record_curve = false;
  cfg.risk.max_orders_per_second = 1'000'000'000u;

  Journal journal;
  Journal::Config jcfg;
  jcfg.path = path;
  jcfg.append = false;
  CHECK(journal.open(jcfg));

  Engine engine(cfg);
  engine.set_journal(&journal);
  // Close the file out from under the engine, standing in for a full disk or a
  // revoked mount.
  journal.close();

  SyntheticFeed::Params fp;
  fp.total_events = 60'000;
  fp.seed = 0x1234ABCDULL;
  SyntheticFeed feed(fp);
  EngineStats stats = engine.run(feed);

  CHECK(stats.journal_failures > 0);
  CHECK(engine.risk().halted());
  CHECK(engine.stop_requested());
}

TEST(oms_ids_are_unique_across_restarts) {
    OrderManager first;
    ClOrdId last = 0;
    for (int i = 0; i < 10; ++i) last = first.create(make_journal_order(), 1);

    // A fresh manager told where the previous one stopped must not reissue.
    OrderManager second;
    second.set_next_id(last + 1);
    CHECK_EQ(second.create(make_journal_order(), 1), last + 1);

    // set_next_id never rewinds: those ids are already on the wire.
    second.set_next_id(2);
    CHECK(second.create(make_journal_order(), 1) > last + 1);
}

TEST(oms_refuses_to_reuse_a_live_id) {
    OrderManager oms;
    CHECK_EQ(oms.create_with_id(7, make_journal_order(), 1), ClOrdId(7));
    // A duplicate would silently merge two distinct orders into one record.
    CHECK_EQ(oms.create_with_id(7, make_journal_order(), 1), ClOrdId(0));
    CHECK_EQ(oms.create_with_id(0, make_journal_order(), 1), ClOrdId(0));
    // Explicit ids advance the sequence, so a later create() cannot collide.
    CHECK_EQ(oms.create(make_journal_order(), 1), ClOrdId(8));
}

TEST(journal_replays_correctly_across_several_sessions) {
    // Regression: client order ids used to restart at 1 each session, so from
    // the second session onward every execution report referred to an id the
    // replay had already assigned to a different order. The whole session
    // replayed as unacknowledged orders that were in fact filled and closed.
    const std::string path = scratch("journal_sessions.jrn");
    remove_file(path);

    ClOrdId next = 1;
    for (int session = 0; session < 3; ++session) {
        Journal j;
        Journal::Config cfg;
        cfg.path = path;
        cfg.append = true;
        CHECK(j.open(cfg));
        j.record_session_start(session * 1000);

        for (int k = 0; k < 5; ++k) {
            const ClOrdId id = next++;
            j.record_order_sent(id, journal_order(0, Side::Buy, 10000, 10), session * 1000 + k);
            j.record_exec_report(journal_report(id, ExecType::Acked, 0, Side::Buy, 0, 0));
            j.record_exec_report(journal_report(id, ExecType::Fill, 0, Side::Buy, 10000, 10));
        }
        j.record_session_end(session * 1000 + 999);
        j.close();

        // After every session, replay must show everything closed.
        const RecoveredState mid = recover_from_journal(path);
        CHECK(mid.ok);
        CHECK_EQ(mid.open_orders.size(), std::size_t(0));
        CHECK_EQ(mid.next_cl_ord_id, next - 1);
    }

    const RecoveredState state = recover_from_journal(path);
    CHECK_EQ(state.position(0), std::int64_t(150));  // 3 sessions x 5 orders x 10
    CHECK_EQ(state.open_orders.size(), std::size_t(0));
    CHECK(state.clean_shutdown);
}

TEST(engine_continues_order_ids_across_a_restart) {
    // End to end: two engine runs sharing one journal must not produce
    // overlapping client order ids, and the journal must replay clean.
    const std::string path = scratch("journal_engine_restart.jrn");
    remove_file(path);

    EngineConfig cfg;
    cfg.threaded = false;
    cfg.fast_window = 3;
    cfg.slow_window = 8;
    cfg.record_curve = false;
    cfg.risk.max_orders_per_second = 1000000000u;

    ClOrdId carried = 1;
    for (int run = 0; run < 3; ++run) {
        Journal journal;
        Journal::Config jcfg;
        jcfg.path = path;
        jcfg.append = true;
        CHECK(journal.open(jcfg));

        Engine engine(cfg);
        engine.set_journal(&journal);
        engine.oms().set_next_id(carried);

        SyntheticFeed::Params fp;
        fp.total_events = 30000;
        fp.seed = 0xA5A5 + static_cast<std::uint64_t>(run);
        SyntheticFeed feed(fp);
        EngineStats stats = engine.run(feed);
        CHECK(stats.orders_sent > 0);

        journal.record_session_end(now_ns());
        journal.close();

        const RecoveredState state = recover_from_journal(path);
        CHECK(state.ok);
        // Nothing is left looking live at the venue after a clean session.
        CHECK_EQ(state.open_orders.size(), std::size_t(0));
        CHECK(state.next_cl_ord_id >= carried);
        carried = state.next_cl_ord_id + 1;
    }
}
