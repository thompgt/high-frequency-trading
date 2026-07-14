"""Entrypoint wiring: YFinanceSource -> RingBuffer -> StrategyEngine ->
PaperExecutionVenue, plus periodic latency summary logging.

Run with: python -m hft.main --symbols AAPL MSFT --poll-interval 2
"""

from __future__ import annotations

import argparse
import asyncio
import logging
from pathlib import Path

from hft.config import Config
from hft.core.engine import StrategyEngine
from hft.core.ringbuffer import RingBuffer
from hft.core.strategy import MovingAverageCrossoverStrategy
from hft.data.base import Tick
from hft.data.yfinance_source import YFinanceSource
from hft.execution.paper import PaperExecutionVenue
from hft.metrics.timing import LatencyRecorder

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
logger = logging.getLogger(__name__)


def parse_args(argv: list[str] | None = None) -> Config:
    parser = argparse.ArgumentParser(description="HFT research pipeline (yfinance-backed)")
    parser.add_argument("--symbols", nargs="+", default=None)
    parser.add_argument("--poll-interval", type=float, default=None)
    parser.add_argument("--fast-window", type=int, default=None)
    parser.add_argument("--slow-window", type=int, default=None)
    parser.add_argument("--summary-interval", type=float, default=None)
    parser.add_argument("--latency-csv", type=str, default=None)
    args = parser.parse_args(argv)

    cfg = Config()
    if args.symbols is not None:
        cfg.symbols = args.symbols
    if args.poll_interval is not None:
        cfg.poll_interval_s = args.poll_interval
    if args.fast_window is not None:
        cfg.fast_window = args.fast_window
    if args.slow_window is not None:
        cfg.slow_window = args.slow_window
    if args.summary_interval is not None:
        cfg.summary_interval_s = args.summary_interval
    if args.latency_csv is not None:
        cfg.latency_csv_path = args.latency_csv
    return cfg


async def _ingest_loop(source: YFinanceSource, symbols: list[str], buffer: RingBuffer[Tick]) -> None:
    async for tick in source.stream(symbols):
        buffer.push(tick)


async def _summary_loop(recorder: LatencyRecorder, interval_s: float) -> None:
    while True:
        await asyncio.sleep(interval_s)
        recorder.log_summary()


async def run(cfg: Config) -> None:
    buffer: RingBuffer[Tick] = RingBuffer(cfg.buffer_capacity)
    source = YFinanceSource(poll_interval_s=cfg.poll_interval_s)
    strategy = MovingAverageCrossoverStrategy(fast_window=cfg.fast_window, slow_window=cfg.slow_window)
    venue = PaperExecutionVenue(slippage_bps=cfg.slippage_bps, fee_bps=cfg.fee_bps)
    recorder = LatencyRecorder()
    engine = StrategyEngine(buffer, strategy, venue, recorder, order_quantity=cfg.order_quantity)

    logger.info("starting pipeline for symbols=%s poll_interval=%.2fs", cfg.symbols, cfg.poll_interval_s)

    tasks = [
        asyncio.create_task(_ingest_loop(source, cfg.symbols, buffer)),
        asyncio.create_task(engine.run_forever()),
        asyncio.create_task(_summary_loop(recorder, cfg.summary_interval_s)),
    ]
    try:
        await asyncio.gather(*tasks)
    finally:
        for t in tasks:
            t.cancel()
        if cfg.latency_csv_path:
            recorder.dump_csv(Path(cfg.latency_csv_path))
        logger.info(
            "shutdown: positions=%s realized_pnl=%.2f dropped_ticks=%d",
            venue.positions,
            venue.realized_pnl,
            buffer.dropped,
        )


def main() -> None:
    cfg = parse_args()
    try:
        asyncio.run(run(cfg))
    except KeyboardInterrupt:
        logger.info("interrupted, shutting down")


if __name__ == "__main__":
    main()
