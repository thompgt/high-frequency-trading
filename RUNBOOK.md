# Runbook

What to do when the engine stops, refuses to start, or starts telling you
something is wrong.

The organising principle throughout: **the engine fails closed.** Every
mechanism here prefers not trading to trading on state it cannot vouch for.
When it stops, it is because continuing was the more dangerous option — so
the question is never "how do I make it start again", it is "what did it
find".

---

## Starting up

```bash
cd cpp
./build/hft_engine --config config/engine.conf --journal /var/lib/hft/engine.jrn
```

Startup order, and what can stop it at each point:

| Step | Fails with | Meaning |
|---|---|---|
| 1. Parse config + CLI | exit 3 | Unknown key, malformed value, or contradictory settings. The message names the key. |
| 2. Validate instruments | exit 3 | A band the book cannot index, a tick grid with no valid price, a duplicate symbol. |
| 3. Replay the journal | exit 4 | The journal is unreadable or was written by a different format version. |
| 4. Check what was left behind | **exit 6** | The previous session did not finish cleanly. **See below.** |
| 5. Open the journal, run | exit 4/5 | Cannot write the journal, or a runtime fault. |

Before going live, confirm the configuration that is actually in effect:

```bash
./build/hft_engine --config config/engine.conf --print-config
```

This prints the merged result of the file and every `--set`, which is what
you want in the incident log rather than a guess at what was intended.

---

## Exit code 6: refusing to start

```
error: refusing to start -- the previous session left state that needs reconciling:
  unclean shutdown : yes
  open orders      : 3 (may still be live at the venue)
  corrupt records  : 0
  venue state      : not available
```

**Do not just add `--allow-unclean-start`.** This is the one exit code that
means a human has to look, and it is deliberately distinct from every other
failure so that an automated supervisor cannot restart into the same
situation in a loop.

The venue still remembers your position and your resting orders even though
the process does not. Quoting on top of orders you have forgotten is how a
crash turns into a loss.

**1. Ask the journal what it knows:**

```bash
./build/hft_engine --recover /var/lib/hft/engine.jrn
```

```
records replayed    : 41822
clean shutdown      : NO -- previous session did not finish cleanly
torn tail           : 56 byte(s) discarded (a write was in flight when the process died)
realized pnl        : -211.40
position symbol 0   : 1200
OPEN ORDERS         : 3 -- these may still be live at the venue; reconcile before quoting
  cl_ord_id=8871 symbol=0 SELL 400 @ 100.83 state=NEW
  cl_ord_id=8872 symbol=0 BUY 200 @ 100.11 state=PENDING_NEW
  cl_ord_id=8873 symbol=0 SELL 600 @ 100.90 state=PARTIALLY_FILLED
```

**2. Read the order states — they are not equally bad:**

| State | What it means | What to do |
|---|---|---|
| `NEW` | The venue acknowledged it. It is almost certainly resting right now. | Cancel it at the venue, or adopt it deliberately. |
| `PARTIALLY_FILLED` | Acknowledged, partly done, remainder still working. | Same, and expect the position to be larger than the journal shows if it filled further after the crash. |
| `PENDING_NEW` | **The worst case.** We sent it and never heard back. It may be live, or it may have died on the wire. | Query the venue by client order id. Never assume either way. |

**3. Reconcile against the venue.** Write down what the venue reports — from
its drop copy, its order-status API, or its terminal — into a file, one record
per line:

```
# symbol is the instrument name; prices are in ticks, as everywhere else
ORDER,<cl_ord_id>,<venue_order_id>,<symbol>,<BUY|SELL>,<price>,<qty>,<filled>,<leaves>
POSITION,<symbol>,<signed position>

ORDER,8871,55210,SYNTH,SELL,10083,400,0,400
POSITION,SYNTH,1200
```

List **every** order the venue is resting and **every** position it holds, not
just the ones you expect. An order you leave out is an order the engine will
report as an orphan, which is the correct and safe direction to be wrong in.

**4. Restart with it, and let the engine do the comparison:**

```bash
./build/hft_engine --config config/engine.conf --journal /var/lib/hft/engine.jrn \
                   --venue-state /var/lib/hft/venue.txt
```

If everything agrees, the engine starts, **adopts** the orders that are still
resting so they count toward every risk limit, and says so:

```
--- reconciliation against /var/lib/hft/venue.txt ---
venue state    : available
open orders    : 3 ours, 3 theirs, 3 agreed
discrepancies  : 0
reconciled clean against the venue: 3 order(s) still resting, adopting them.
```

If anything disagrees, it exits 6 again with the disagreements named:

| Break | What it means | What to do |
|---|---|---|
| `ORPHAN_AT_VENUE` | **The worst one.** The venue is resting an order we have no record of. No limit sees it and no cancel of ours reaches it. | Cancel it at the venue by hand, then re-pull the venue state. |
| `MISSING_AT_VENUE` | We think an order is working; the venue is not resting it. It either never arrived or is already done. | Check the venue's fill history — if it filled, the position line is what matters. |
| `QUANTITY_MISMATCH` | Both sides know the order, the leaves differ. A fill report was lost. | Trust the venue. Correct the position line and re-pull. |
| `TERMS_MISMATCH` | The same id refers to orders with different price or side. | Stop. This means an id was reused; do not restart until you know why. |
| `POSITION_MISMATCH` | Our replayed position is not the venue's. | The venue is the authority. Investigate before quoting; a wrong starting position makes every subsequent limit wrong. |

**5. The override is a last resort:**

```bash
./build/hft_engine --config config/engine.conf --journal /var/lib/hft/engine.jrn \
                   --allow-unclean-start
```

`--allow-unclean-start` **skips** the check rather than passing it. Use it only
when you have reconciled by hand and are certain, and record why in the
incident log.

### `torn tail` versus `CORRUPT records`

These look similar and mean very different things.

- **Torn tail** — a partial record at the *end* of the file. This is the write
  that was in flight when the process died. It is expected after any crash and
  costs you at most one event.
- **CORRUPT records** — a record that failed its checksum with *valid data
  after it*. This is not a crash artefact, it is storage corruption or a bug.
  The recovered state is missing something and you cannot know what. Treat the
  recovered position as untrustworthy and reconcile everything against the
  venue.

---

## Halts

A halt is sticky by design. A limit that has been breached stays breached
until someone releases it — a strategy that has already lost the day's budget
does not get to resume because the next tick happened to be favourable.

The halt reason is journalled and fsynced at the moment it happens, so "why
did we stop?" is always answerable.

| Reason | Trigger | What it means |
|---|---|---|
| `drawdown_breach` | Peak-to-trough equity exceeded `max_drawdown` | The strategy is losing. This is the limit working. Do not raise it to get back in. |
| `daily_order_limit` | `max_daily_orders` reached | Usually a runaway strategy loop, occasionally a genuinely busy day. Check `orders sent` against the throughput you expected. |
| `order_timeout` | An order went past `ack_timeout_ms` with no response | We do not know whether it is resting at the venue or died on the wire. Query the venue by the `cl_ord_id` in the error log before restarting. |
| `manual` | Feed fault, or a journal write failure | See the two sections below — the cause is in the run report. |

On halt the engine stops taking new risk and, if `flatten_on_exit` is set,
closes out inventory on shutdown. Flattening deliberately **bypasses the
pre-trade gate**: otherwise the position limit that halted you would also
block the exit.

---

## The feed faulted

```
--- market data session ---
sequence gaps       : 1  (500 message(s) lost, largest gap 500)
session state       : FAULTED -- the book is missing updates and cannot be trusted
```

The engine stopped trading because it knows the book is wrong. Not stale —
wrong, and permanently so: a missed add leaves phantom liquidity in the book
and a missed cancel leaves depth that is not there, and no later message
corrects either.

**This is not a condition you can clear by restarting.** Restarting rebuilds
the book from the point you restart, which is fine, but it does not tell you
why messages were lost. Check, in order:

1. **The network path.** Dropped multicast, a saturated NIC ring buffer, a
   switch under load. Lost messages are almost always a transport problem.
2. **The consumer.** If `ring buffer drops` is non-zero, the engine could not
   keep up and dropped them itself — that is a capacity problem, not a network
   one.
3. **The feed itself.** Rare, but arbitrated feeds do have genuine outages.

`duplicates` is not a fault and needs no action — arbitrated A/B feeds
deliver every message twice by design, and the engine drops the second copy
before it reaches the book.

`unsequenced` means the feed supplied no sequence numbers while
`feed_require_sequence = true`. That is a configuration mismatch, not lost
data. Either the feed genuinely has no sequencing (set it to `false` and
accept that you are trusting the transport completely) or something upstream
is stripping it.

### Staleness

```
stale events        : 1
```

No message arrived within `feed_stale_ms`. A silent feed and a calm market are
indistinguishable from inside the process, and only one of them is safe to
quote against.

`feed_stale_ms` defaults to 0 (disabled) on purpose: the right threshold
depends entirely on how busy the instrument is, and a wrong value is worse
than none — too tight and you halt on every quiet minute, too loose and it
never fires. Set it from the observed inter-message gap distribution for the
instrument, well above the p99.9.

---

## Journal write failures

```
warning: journal write failed
```

The engine halts and stops. This is not a degraded mode: an engine that
cannot record what it did cannot be recovered afterwards, so from the first
failed write onward a crash would leave state nobody can reconstruct.

Usual causes: the disk is full, the mount went read-only, or a network
filesystem stalled. Fix the storage before restarting. Note that the journal
should be on **local** storage — a network filesystem puts a network round
trip on the order path and can stall unboundedly.

### Choosing a sync policy

| Policy | Survives process crash | Survives power loss | Cost |
|---|---|---|---|
| `on_write` (default) | Yes | No | Negligible |
| `interval` | Yes | Loses at most `journal_sync_interval` records | Amortised |
| `always` | Yes | Yes | ~1ms per record — caps you near 1000 orders/sec |

`on_write` is the honest default. Data that has reached the OS survives the
process dying, because the kernel did not die; only power loss needs `fsync`.
If you choose `always`, measure what it costs you first.

---

## Metrics to alarm on

From `metrics.json` in `--out-dir`. Ranked by how quickly you want to know.

**Page someone immediately:**

| Field | Threshold | Why |
|---|---|---|
| `orders.breaks` | `> 0` | Our view of an order and the venue's have diverged. Everything downstream of that is suspect. |
| `feed.faulted` | `true` | The book is missing updates and cannot be traded on. |
| `risk.halted` | `true` | Trading has stopped. `halt_reason` says why. |
| `trading.untracked_rejects` | `> 0` | Orders were suppressed because the OMS had no slot. Something is opening far more orders than expected. |

**Investigate the same day:**

| Field | Threshold | Why |
|---|---|---|
| `feed.gaps` | `> 0` | Even a tolerated gap means messages were lost. |
| `feed.stale_events` | `> 0` | The feed went quiet. |
| `orders.expired` / `trading.timed_out_orders` | `> 0` | Orders went unacknowledged past the timeout — venue latency or a dropped session. With `halt_on_order_timeout` on (the default) this also halts, so it will normally arrive alongside `risk.halted`. |
| `run.dropped_ticks` | `> 0` | The engine could not keep up with its own feed. |
| `orders.adopted` | `> 0` | This session inherited live orders from the last one. Expected after a reconciled restart; unexplained otherwise. |
| `risk.rejects_by_reason.*` | any sustained non-zero | Each reason names a specific misconfiguration or strategy bug. |

**Watch the trend, do not alarm:**

`latency.stages.*.p99` — a rising p99 with unchanged throughput usually means
allocation or contention has crept onto the hot path.

### Reject reasons, decoded

| Reason | What it is telling you |
|---|---|
| `order_quantity_too_large` / `order_notional_too_large` | Fat finger. Almost always a strategy bug, not a limit that is too tight. |
| `price_outside_collar` | The strategy priced far from the reference. Either the price feed is corrupt or the strategy is. |
| `position_limit_exceeded` | Inventory bound hit, **including in-flight orders**. If this fires with a flat position, the engine has a lot of unanswered orders in the air. |
| `throttle_exceeded` | Order rate limit. Benchmarks trip this constantly because they replay hours of flow in seconds — raise it for benchmarking, keep it realistic when operating. |
| `kill_switch_engaged` | Already halted; every subsequent order is refused. Look at `halt_reason`, not at this. |

---

## Shutdown

`SIGINT` / `SIGTERM` request a cooperative stop. The signal handler only sets
a flag — calling into the engine from a signal context is not safe — and a
watcher thread turns it into a stop request.

A clean shutdown then:

1. drains the ring buffer,
2. flattens inventory if `flatten_on_exit` is set,
3. writes a position/PnL checkpoint to the journal,
4. writes the end-of-session marker, fsynced,
5. writes metrics and CSVs,
6. flushes and joins the async logger.

Step 4 is what lets the next start proceed without exit code 6. If you
`SIGKILL` the process, that record is never written and the next start will
(correctly) demand reconciliation.

Give the engine time to finish. `SIGTERM` then `SIGKILL` after a short
timeout, which is what most supervisors do by default, can cut off the
checkpoint — allow at least a few seconds.
