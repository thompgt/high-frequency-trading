"""Example strategy: moving-average crossover.

Kept deliberately simple -- the point of this scaffold is the pipeline and
its latency instrumentation, not alpha. Swap this module out for real
signal logic without touching the engine's plumbing.
"""

from __future__ import annotations

import abc
from collections import deque
from dataclasses import dataclass
from typing import Literal

from hft.data.base import Tick

Side = Literal["BUY", "SELL"]


@dataclass(frozen=True, slots=True)
class Signal:
    symbol: str
    side: Side
    price: float
    tick_ingest_ts_ns: int


class Strategy(abc.ABC):
    @abc.abstractmethod
    def on_tick(self, tick: Tick) -> Signal | None:
        """Return a Signal if this tick triggers one, else None."""
        raise NotImplementedError


class MovingAverageCrossoverStrategy(Strategy):
    def __init__(self, fast_window: int = 5, slow_window: int = 20) -> None:
        if fast_window >= slow_window:
            raise ValueError("fast_window must be smaller than slow_window")
        self.fast_window = fast_window
        self.slow_window = slow_window
        self._history: dict[str, deque[float]] = {}
        self._last_state: dict[str, bool] = {}  # True = fast above slow

    def on_tick(self, tick: Tick) -> Signal | None:
        prices = self._history.setdefault(tick.symbol, deque(maxlen=self.slow_window))
        prices.append(tick.price)
        if len(prices) < self.slow_window:
            return None

        fast_avg = sum(list(prices)[-self.fast_window :]) / self.fast_window
        slow_avg = sum(prices) / len(prices)
        fast_above = fast_avg > slow_avg

        prev = self._last_state.get(tick.symbol)
        self._last_state[tick.symbol] = fast_above

        if prev is None or prev == fast_above:
            return None  # no crossover

        side: Side = "BUY" if fast_above else "SELL"
        return Signal(
            symbol=tick.symbol,
            side=side,
            price=tick.price,
            tick_ingest_ts_ns=tick.ingest_ts_ns,
        )
