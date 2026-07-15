import asyncio
import time

from hft.core.engine import StrategyEngine
from hft.core.ringbuffer import RingBuffer
from hft.core.strategy import MovingAverageCrossoverStrategy
from hft.data.base import Tick
from hft.execution.paper import PaperExecutionVenue
from hft.metrics.timing import LatencyRecorder


def make_tick(price: float) -> Tick:
    return Tick(symbol="AAPL", price=price, volume=10, source_ts_ns=time.time_ns())


def test_engine_processes_ticks_and_records_latency():
    buffer: RingBuffer[Tick] = RingBuffer(capacity=16)
    strategy = MovingAverageCrossoverStrategy(fast_window=2, slow_window=4)
    venue = PaperExecutionVenue()
    recorder = LatencyRecorder()
    engine = StrategyEngine(buffer, strategy, venue, recorder)

    prices = [100, 99, 98, 97, 105, 110, 111, 112]
    for p in prices:
        buffer.push(make_tick(p))

    async def scenario():
        task = asyncio.create_task(engine.run_forever())
        # give the engine a chance to drain the buffer
        for _ in range(50):
            if len(buffer) == 0:
                break
            await asyncio.sleep(0.01)
        engine.stop()
        buffer.push(make_tick(999))  # unblock the final pop() wait
        await asyncio.wait_for(task, timeout=1)

    asyncio.run(scenario())

    summary = recorder.summary()
    assert summary["count"] >= len(prices)
    assert "AAPL" in venue.positions
