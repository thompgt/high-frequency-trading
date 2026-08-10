"""Drive the real pipeline with a synthetic feed so the Grafana dashboard has
something to show.

The live yfinance path is the honest data source, but it is a poor demo: it
polls once a second per symbol, and a moving-average crossover needs both a
full slow window of ticks and an actual crossing, so a quiet tape (or a
closed market) produces a dashboard of flat zero lines. This script keeps
every real component -- RingBuffer, MovingAverageCrossoverStrategy,
StrategyEngine, PaperExecutionVenue -- and swaps in an oscillating price
source that crosses often enough to exercise every panel.

    python scripts/demo_metrics_load.py --duration 120

Nothing here is used by the production entrypoint; hft/main.py --metrics is
the real thing.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import math
import random
import time
from typing import AsyncIterator, Sequence

from hft.core.engine import StrategyEngine
from hft.core.ringbuffer import RingBuffer
from hft.core.strategy import MovingAverageCrossoverStrategy
from hft.data.base import MarketDataSource, Tick
from hft.execution.base import Fill, Order
from hft.execution.paper import PaperExecutionVenue
from hft.metrics import prom
from hft.metrics.timing import LatencyRecorder

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
logger = logging.getLogger("demo")


class OscillatingSource(MarketDataSource):
    """Sine wave plus noise -- guarantees regular moving-average crossovers."""

    def __init__(self, tick_interval_s: float = 0.02, period_s: float = 6.0, amplitude_pct: float = 0.02) -> None:
        self.tick_interval_s = tick_interval_s
        self.period_s = period_s
        self.amplitude_pct = amplitude_pct

    async def stream(self, symbols: Sequence[str]) -> AsyncIterator[Tick]:
        bases = {sym: 100.0 + 50.0 * i for i, sym in enumerate(symbols)}
        t0 = time.monotonic()
        while True:
            elapsed = time.monotonic() - t0
            for sym, base in bases.items():
                phase = 2 * math.pi * elapsed / self.period_s
                price = base * (1 + self.amplitude_pct * math.sin(phase)) + random.gauss(0, base * 0.0005)
                yield Tick(symbol=sym, price=price, volume=random.randint(100, 5000), source_ts_ns=time.time_ns())
            await asyncio.sleep(self.tick_interval_s)


class FlakyPaperVenue(PaperExecutionVenue):
    """Paper venue that intermittently raises, so the reject-by-reason panel
    has data. A real broker adapter fails this way; the plain paper venue
    never does, which makes that panel permanently empty in a demo."""

    def __init__(self, reject_rate: float = 0.0, **kwargs) -> None:
        super().__init__(**kwargs)
        self.reject_rate = reject_rate

    async def submit(self, order: Order, reference_price: float) -> Fill:
        if random.random() < self.reject_rate:
            raise random.choice([ConnectionError, TimeoutError, PermissionError])("simulated venue failure")
        return await super().submit(order, reference_price)


async def _ingest(source: MarketDataSource, symbols: list[str], buffer: RingBuffer[Tick]) -> None:
    async for tick in source.stream(symbols):
        buffer.push(tick)
        prom.record_tick(tick.symbol, tick.price)
        prom.record_buffer_state(len(buffer), buffer.dropped)


async def main_async(args: argparse.Namespace) -> None:
    buffer: RingBuffer[Tick] = RingBuffer(1024)
    source = OscillatingSource(tick_interval_s=args.tick_interval, period_s=args.period)
    strategy = MovingAverageCrossoverStrategy(fast_window=args.fast_window, slow_window=args.slow_window)
    venue = FlakyPaperVenue(reject_rate=args.reject_rate, slippage_bps=1.0, fee_bps=0.5)
    recorder = LatencyRecorder(on_sample=prom.observe_latency_sample)
    engine = StrategyEngine(buffer, strategy, venue, recorder, order_quantity=10)

    prom.start_exporter(args.metrics_port)
    logger.info("driving %s for %.0fs; metrics on http://localhost:%d/metrics", args.symbols, args.duration, args.metrics_port)

    tasks = [
        asyncio.create_task(_ingest(source, args.symbols, buffer)),
        asyncio.create_task(engine.run_forever()),
    ]
    try:
        await asyncio.wait(tasks, timeout=args.duration if args.duration > 0 else None)
    finally:
        for t in tasks:
            t.cancel()
        logger.info(
            "done: positions=%s realized_pnl=%.2f dropped=%d latency=%s",
            venue.positions,
            venue.realized_pnl,
            buffer.dropped,
            recorder.summary(),
        )


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--symbols", nargs="+", default=["DEMO-A", "DEMO-B"])
    p.add_argument("--duration", type=float, default=120.0, help="seconds; 0 runs until interrupted")
    p.add_argument("--tick-interval", type=float, default=0.02)
    p.add_argument("--period", type=float, default=6.0, help="seconds per price oscillation")
    p.add_argument("--fast-window", type=int, default=5)
    p.add_argument("--slow-window", type=int, default=20)
    p.add_argument("--reject-rate", type=float, default=0.1, help="fraction of orders the demo venue fails")
    p.add_argument("--metrics-port", type=int, default=prom.DEFAULT_PORT)
    return p.parse_args()


if __name__ == "__main__":
    try:
        asyncio.run(main_async(parse_args()))
    except KeyboardInterrupt:
        pass
