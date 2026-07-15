# high-frequency-trading

A low-latency-oriented market data / signal / paper-trading pipeline, currently
backed by [yfinance](https://github.com/ranaroussi/yfinance).

## Important limitation: this is not actually HFT (yet)

`yfinance` is an unofficial scraper of Yahoo Finance endpoints. It has no
SLA, gets rate-limited/IP-blocked under sustained polling, and "real-time"
fields are frequently delayed by seconds. It also has no order-execution
API. **This is a research and paper-trading pipeline, not a low-latency
exchange feed.**

The architecture is deliberately built so a real feed and broker can be
dropped in later without a rewrite:

- `hft/data/base.py` defines `MarketDataSource`, an async streaming
  interface. `YFinanceSource` is one implementation; a future
  `PolygonSource` / `FixSource` / broker WebSocket implementation is another.
- `hft/execution/base.py` defines `ExecutionVenue`. `PaperExecutionVenue`
  simulates fills in memory; a real broker (Alpaca, IBKR, a FIX gateway)
  implements the same interface.

Nothing in `hft/core/` (the ring buffer, strategy, or engine) needs to
change when either side is swapped.

## Architecture

```
YFinanceSource --stream()--> RingBuffer --pop()--> StrategyEngine --submit()--> ExecutionVenue
                                                          |
                                                          v
                                                   LatencyRecorder
```

- **`hft/data/`** — market data source interface + yfinance polling
  implementation. Polls `fast_info` on a background thread with jittered
  exponential backoff to survive rate limiting.
- **`hft/core/ringbuffer.py`** — a preallocated single-producer/
  single-consumer ring buffer between ingestion and the strategy engine.
  Drops the oldest tick on overflow rather than blocking the producer —
  in a trading pipeline, stalling ingestion to preserve a stale tick is
  the wrong tradeoff.
- **`hft/core/strategy.py`** — example moving-average crossover strategy.
  Swap this out for real signal logic; the engine doesn't care.
- **`hft/core/engine.py`** — drains the ring buffer, runs the strategy,
  forwards signals to the execution venue, and records stage-boundary
  timestamps.
- **`hft/execution/`** — execution venue interface + in-memory paper
  simulator (slippage/fee modeling, position and realized-PnL tracking).
- **`hft/metrics/timing.py`** — nanosecond ingest→signal and
  signal→order latency tracking, with p50/p99 summaries and CSV export.
  No network I/O on the hot path.

## Running it

```bash
pip install -r requirements.txt
python -m hft.main --symbols AAPL MSFT --poll-interval 2
```

Useful flags: `--fast-window`, `--slow-window`, `--summary-interval`,
`--latency-csv <path>` (dumps per-tick latency samples on shutdown).

Or via Docker:

```bash
docker build -t hft .
docker run --rm hft --symbols AAPL MSFT
```

## Tests

```bash
pip install -r requirements.txt
pytest -q
```

All tests mock `yfinance` — none make live network calls.

## Path to real low latency

If/when this needs to be genuinely low-latency:

1. Replace `YFinanceSource` with a real feed (Polygon, Databento, IEX,
   or a broker's WebSocket/FIX market data API) behind the same
   `MarketDataSource` interface.
2. Replace `PaperExecutionVenue` with a real broker integration behind
   `ExecutionVenue`.
3. Move the ring buffer to shared memory
   (`multiprocessing.shared_memory`) if ingestion and the engine are
   split across processes/machines for isolation.
4. Move hot-path numeric work (indicators, order-book math) into Numba
   or a small Rust/C++ extension (PyO3/pybind11); pin the engine to a
   dedicated core.
5. Use the `LatencyRecorder` output to find the actual bottleneck before
   optimizing — measure, don't guess.
