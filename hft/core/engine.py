"""Strategy engine: drains the ring buffer, runs the strategy, and hands any
resulting signal to the execution venue -- recording stage-boundary
timestamps along the way for latency measurement."""

from __future__ import annotations

import logging
import time

from hft.core.ringbuffer import RingBuffer
from hft.core.strategy import Strategy
from hft.data.base import Tick
from hft.execution.base import ExecutionVenue, Order
from hft.metrics.timing import LatencyRecorder

logger = logging.getLogger(__name__)


class StrategyEngine:
    def __init__(
        self,
        buffer: RingBuffer[Tick],
        strategy: Strategy,
        venue: ExecutionVenue,
        recorder: LatencyRecorder | None = None,
        order_quantity: int = 1,
    ) -> None:
        self.buffer = buffer
        self.strategy = strategy
        self.venue = venue
        self.recorder = recorder or LatencyRecorder()
        self.order_quantity = order_quantity
        self._running = False

    async def run_forever(self) -> None:
        self._running = True
        while self._running:
            tick = await self.buffer.pop()
            signal_start_ns = time.time_ns()
            signal = self.strategy.on_tick(tick)
            signal_end_ns = time.time_ns()

            if signal is None:
                self.recorder.record_tick(tick.ingest_ts_ns, signal_start_ns, signal_end_ns)
                continue

            order = Order(symbol=signal.symbol, side=signal.side, quantity=self.order_quantity)
            order_start_ns = time.time_ns()
            fill = await self.venue.submit(order, reference_price=signal.price)
            order_end_ns = time.time_ns()

            self.recorder.record_signal(
                tick_ingest_ns=tick.ingest_ts_ns,
                signal_start_ns=signal_start_ns,
                signal_end_ns=signal_end_ns,
                order_start_ns=order_start_ns,
                order_end_ns=order_end_ns,
            )
            logger.info("filled %s %s x%d @ %.4f", fill.side, fill.symbol, fill.quantity, fill.price)

    def stop(self) -> None:
        self._running = False
