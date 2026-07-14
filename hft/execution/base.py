"""Execution-venue interface. A paper simulator today; a real broker API
(Alpaca, IBKR, a FIX gateway) later implements the same interface so the
strategy engine never needs to change."""

from __future__ import annotations

import abc
import time
from dataclasses import dataclass, field
from typing import Literal

Side = Literal["BUY", "SELL"]


@dataclass(frozen=True, slots=True)
class Order:
    symbol: str
    side: Side
    quantity: int
    created_ts_ns: int = field(default_factory=time.time_ns)


@dataclass(frozen=True, slots=True)
class Fill:
    symbol: str
    side: Side
    quantity: int
    price: float
    filled_ts_ns: int = field(default_factory=time.time_ns)


class ExecutionVenue(abc.ABC):
    @abc.abstractmethod
    async def submit(self, order: Order, reference_price: float) -> Fill:
        """Submit an order and return its fill."""
        raise NotImplementedError
