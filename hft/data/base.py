"""Data-source interface. Every feed (yfinance today, a real exchange feed
later) implements this and nothing downstream needs to change."""

from __future__ import annotations

import abc
import time
from dataclasses import dataclass, field
from typing import AsyncIterator, Sequence


@dataclass(frozen=True, slots=True)
class Tick:
    symbol: str
    price: float
    volume: int | None
    source_ts_ns: int  # timestamp reported by the upstream source, if any
    ingest_ts_ns: int = field(default_factory=time.time_ns)


class MarketDataSource(abc.ABC):
    """Async streaming interface for market data feeds."""

    @abc.abstractmethod
    def stream(self, symbols: Sequence[str]) -> AsyncIterator[Tick]:
        """Yield ticks for the given symbols indefinitely."""
        raise NotImplementedError

    async def __aenter__(self) -> "MarketDataSource":
        return self

    async def __aexit__(self, *exc_info) -> None:
        return None
