# high-frequency-trading

A low-latency trading engine in C++17, with a Python research pipeline
alongside it.

The engine runs entirely offline against a deterministic synthetic feed or a
captured replay file, so `make && ./build/hft_engine` reproduces the same
trades on any machine with a compiler. No network, no dependencies, no
credentials.

## Tech Stack

![C++](https://img.shields.io/badge/C++17-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-064F8C?style=for-the-badge&logo=cmake&logoColor=white)
![Python](https://img.shields.io/badge/Python-3776AB?style=for-the-badge&logo=python&logoColor=white)
![Docker](https://img.shields.io/badge/Docker-2496ED?style=for-the-badge&logo=docker&logoColor=white)
![GitHub Actions](https://img.shields.io/badge/GitHub_Actions-2088FF?style=for-the-badge&logo=githubactions&logoColor=white)
![yfinance](https://img.shields.io/badge/yfinance-6001D2?style=for-the-badge)

## What is honest about this, and what is not

**Real:** the order book, the order lifecycle, the pre-trade risk controls,
crash recovery, and the market-data session validation are built the way
production systems build them, and the reasoning is written down in the
headers rather than assumed. The failure paths are tested harder than the
happy paths.

**Not real:** there is no exchange connectivity. `PaperVenue` simulates fills
against the live book; there is no FIX or binary order-entry session, no
market-data snapshot channel to recover from a gap, and no colocation. The
latency numbers below are from a general-purpose developer machine with no
core pinning, so they measure the code, not a trading system.

The Python pipeline under `hft/` is a research companion backed by
[yfinance](https://github.com/ranaroussi/yfinance), which is an unofficial
scraper with no SLA and no execution API. It is a paper-trading and
prototyping tool, not a feed.

## Architecture

```
                        ┌──────────────┐
  feed ──sequence──────▶│ FeedMonitor  │  gap? duplicate? silent?
   │                    └──────┬───────┘
   │                           │  (a gap means the book is wrong -> halt)
   ▼                           ▼
 SPSC ring ──▶ OrderBook per instrument ──▶ Strategy ──▶ RiskManager ──▶ Venue
                     │                                        ▲            │
                     │                                        │            ▼
              InstrumentRegistry                          OrderManager ◀── exec
              (band, tick, lot)                        (state, in-flight)  reports
                                                             │
                                                             ▼
                                                          Journal ──▶ recovery
                                                                          │
                                          venue drop copy ──▶ reconcile ◀─┘
                                                    (agree? adopt. differ? stop.)
```

Every stage is behind an interface (`MarketDataSource`, `ExecutionVenue`,
`Strategy`, `BookProvider`), so a real feed or broker drops in without
touching anything downstream.

### The pieces

| Component | File | What it is for |
|---|---|---|
| **Order book** | `cpp/include/hft/order_book.hpp` | Price-time priority. Flat level array + intrusive FIFO + three-tier bitmap, so best-bid/ask is O(1) and add/cancel never allocate. |
| **Instruments** | `instrument.hpp` | Per-instrument price band, tick size, lot size, position ceiling. One book each. |
| **Order lifecycle** | `oms.hpp` | Explicit state machine from send to terminal. Supplies in-flight exposure to risk and counts every reconciliation break. |
| **Pre-trade risk** | `risk.hpp` | Fat-finger, price collar, inventory (including in-flight), throttle, drawdown kill switch. Fails closed. |
| **Feed health** | `feed_health.hpp` | Sequence validation, duplicate/reorder rejection, staleness watchdog. |
| **Durability** | `journal.hpp` | CRC32'd append-only journal; recovery rebuilds position and open orders after a crash. |
| **Reconciliation** | `reconcile.hpp` | Compares what we recovered against what the venue says it holds, and names every disagreement. |
| **Ring buffer** | `ring_buffer.hpp` | Lock-free SPSC, cache-line padded, no false sharing. |
| **Latency** | `latency.hpp` | HdrHistogram-style buckets, allocation-free on the record path. |
| **Logging** | `log.hpp` | Async: no formatting and no I/O on the tick path. Drops rather than blocks. |

## Four things that make it operable rather than just fast

These are the parts that separate an engine you can run from a benchmark.

**1. In-flight orders count as risk.** Position alone is not exposure. An
engine that has fired 500 lots and had none come back is carrying 500 lots
that no position-based limit can see, and it will happily fire 500 more.
Every limit is checked against position + working quantity.

**2. A missed market data message stops trading.** The book is built by
applying every message in order, so missing one leaves it not stale but
*wrong* — permanently, because nothing later corrects it. A missed add is
phantom liquidity; a missed cancel is depth that is not there. The correct
response is to re-synchronise from a snapshot; this engine has no snapshot
channel, so it does the honest half and halts.

**3. A crash cannot lose what you own.** Every order is journalled before it
can exist at the venue, and every execution report before it is applied. On
restart, recovery runs before anything can trade. If the last session did not
exit cleanly or left orders that may still be live, the engine **refuses to
start** (exit code 6) until it can reconcile against the venue.

Recovery alone only answers "what did *we* last write down?". The venue is the
authority, so a restart can be handed what the venue reports
(`--venue-state FILE`) and the two are compared. An order the venue is resting
that we know nothing about is the break that matters: it is live, no limit
sees it, and no cancel of ours can reach it. Orders both sides agree on are
adopted into the OMS before the first tick, so they are risk the engine can
see. Everything else stops the start.

**4. Divergence from the venue is counted, not swallowed.** Unknown order ids,
duplicate acks, illegal transitions and overfills are all counted as
reconciliation breaks. A non-zero count in `metrics.json` means our view and
the venue's disagree.

## Running it

```bash
cd cpp
make                     # engine, tests, benchmarks
./build/hft_engine --events 2000000 --out-dir out
```

Or with CMake:

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build --parallel
ctest --test-dir cpp/build --output-on-failure
```

### Useful invocations

```bash
# Trade three instruments, each with its own book, band and tick grid.
./build/hft_engine --set instrument=AAPL:15000:30000 \
                   --set instrument=ESZ5:400000:410000:25:1:250 \
                   --set instrument=PENNY:100:900

# Run with durability on, so a crash is recoverable.
./build/hft_engine --journal out/engine.jrn --out-dir out

# Ask a journal what the last session left behind.
./build/hft_engine --recover out/engine.jrn

# Restart after a crash, reconciling against what the venue reports it holds.
./build/hft_engine --journal out/engine.jrn --venue-state out/venue.txt

# Capture a replay file, then replay it deterministically.
./build/hft_engine --write-replay out/replay.csv --replay-events 200000
./build/hft_engine --replay out/replay.csv

# Every setting, and the effective config after files and flags are merged.
./build/hft_engine --list-settings
./build/hft_engine --config config/engine.conf --print-config
```

### Docker

```bash
docker build -t hft-engine cpp/
docker run --rm -v "$PWD/out:/app/out" -v hft-journal:/var/lib/hft hft-engine \
  --journal /var/lib/hft/engine.jrn --out-dir /app/out
```

The image builds with `-Werror` and runs the unit tests as part of the build,
so an image that fails its own tests never gets created. Keep the journal on a
volume — a journal inside the container layer dies with the container, which
defeats the point of having one.

### Configuration

Configuration is `key = value` in `cpp/config/engine.conf`, with CLI `--set`
applied afterwards. **An unknown key is a hard error**, not a warning: a
typo'd `max_postion_per_symbol` that silently leaves the real limit at its
default is exactly the failure the config layer exists to prevent.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 2 | Bad command line |
| 3 | Bad configuration |
| 4 | I/O failure (could not write artefacts, unreadable journal) |
| 5 | Runtime fault |
| 6 | **The previous session left state needing reconciliation.** Do not restart blindly — see [RUNBOOK.md](RUNBOOK.md). |

### Artefacts

`--out-dir` writes `metrics.json` (throughput, PnL, per-reason reject
breakdown, feed health, order lifecycle, latency percentiles),
`latency_summary.csv`, `latency_histogram.csv`, `fills.csv`, and
`book_snapshot.csv`.

## Operations

[RUNBOOK.md](RUNBOOK.md) covers startup, what each halt means, how to recover
from a crash, and which metrics to alarm on.

## Tests

```bash
cd cpp
make test          # 245 unit tests
make hardened      # UBSan trap mode + _GLIBCXX_DEBUG, works on MinGW
make asan          # ASan + UBSan (Linux; MinGW ships no sanitizer runtime)
make tsan
```

The order book carries a randomised differential test against an
independently-maintained shadow model — matching logic is the one place where
a subtle bug silently corrupts everything downstream. The journal is tested
against the states a real crash leaves behind: truncated mid-record, whole
records lost, a flipped bit mid-file, a missing end-of-session marker.

CI builds both build systems on Linux and Windows, runs ASan/UBSan/TSan, and
asserts the operational behaviour end to end: that a gapped feed halts, that
an unclean restart is refused with exit code 6, and that a replay reproduces
identical trading results.

## Benchmarks

```bash
cd cpp
make bench
```

Numbers are from one developer machine with no core pinning and are not
publishable figures — the benchmark exists to catch regressions and to find
which stage actually dominates. Measure before optimising.

## Python research pipeline

```bash
pip install -r requirements.txt
python -m hft.main --symbols AAPL MSFT --poll-interval 2
pytest -q
```

`notebooks/demo.ipynb` is an executed end-to-end walkthrough with saved
outputs. All Python tests mock `yfinance`; none make live network calls.

The C++ engine's paper venue mirrors `hft/execution/paper.py`'s average-cost
accounting exactly, so both sides agree to the cent over the same fills. (One
divergence was found while porting: `paper.py` carried the old average cost
forward when a fill flipped a position from long to short, double-counting
PnL that had just been realized. No Python test covered a flip. Fixed on both
sides.)

## What it would take to trade real money

Honestly stated, in order:

1. **Exchange connectivity.** A binary or FIX order-entry session with
   heartbeats, cancel-on-disconnect, and sequence recovery. `ExecutionVenue`
   is the seam; nothing above it changes.
2. **A market data snapshot channel.** Today a gap halts the engine because
   there is no way to rebuild the book. With snapshot recovery it could
   re-synchronise and carry on.
3. **A live order-status query.** Startup reconciliation exists and gates the
   restart, but with no order-entry session the venue's side has to be handed
   in as a file. With connectivity it becomes an order-status request, and the
   same comparison runs unchanged.
4. **A real strategy.** The moving-average crossover is a placeholder that
   exists to exercise the pipeline.
5. **Hardware and tuning.** Core pinning, isolated CPUs, huge pages, kernel
   bypass. Only after the `LatencyRecorder` output says where the time
   actually goes.
