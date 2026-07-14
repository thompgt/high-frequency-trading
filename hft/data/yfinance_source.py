"""yfinance-backed MarketDataSource.

IMPORTANT: yfinance is an unofficial scraper of Yahoo Finance endpoints. It
has no SLA, gets IP-blocked under sustained polling, and "real-time" fields
are frequently delayed by seconds. This is a research/paper-trading data
source, not a low-latency exchange feed. Swap in a real feed (Polygon,
Databento, a broker FIX/WebSocket API) by implementing MarketDataSource --
nothing else in the pipeline needs to change.
"""

from __future__ import annotations

import asyncio
import logging
import random
import time
from typing import AsyncIterator, Sequence

import yfinance as yf

from hft.data.base import MarketDataSource, Tick

logger = logging.getLogger(__name__)


class YFinanceSource(MarketDataSource):
    """Polls yfinance fast_info on a fixed interval with jittered backoff."""

    def __init__(
        self,
        poll_interval_s: float = 1.0,
        max_backoff_s: float = 30.0,
        jitter_s: float = 0.25,
    ) -> None:
        self.poll_interval_s = poll_interval_s
        self.max_backoff_s = max_backoff_s
        self.jitter_s = jitter_s

    async def stream(self, symbols: Sequence[str]) -> AsyncIterator[Tick]:
        tickers = {sym: yf.Ticker(sym) for sym in symbols}
        backoff = self.poll_interval_s

        while True:
            loop_start = time.monotonic()
            try:
                for sym, ticker in tickers.items():
                    tick = await asyncio.to_thread(self._fetch_one, sym, ticker)
                    if tick is not None:
                        yield tick
                backoff = self.poll_interval_s  # reset after a clean pass
            except Exception:
                logger.exception("yfinance poll failed, backing off %.1fs", backoff)
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, self.max_backoff_s)
                continue

            elapsed = time.monotonic() - loop_start
            sleep_for = max(0.0, self.poll_interval_s - elapsed)
            sleep_for += random.uniform(0, self.jitter_s)
            await asyncio.sleep(sleep_for)

    @staticmethod
    def _fetch_one(symbol: str, ticker: "yf.Ticker") -> Tick | None:
        info = ticker.fast_info
        price = info.get("lastPrice") if isinstance(info, dict) else info.last_price
        if price is None:
            return None
        volume = info.get("lastVolume") if isinstance(info, dict) else getattr(
            info, "last_volume", None
        )
        return Tick(
            symbol=symbol,
            price=float(price),
            volume=int(volume) if volume is not None else None,
            source_ts_ns=time.time_ns(),
        )
