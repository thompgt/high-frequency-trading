import asyncio

import pytest

from hft.core.ringbuffer import RingBuffer


def test_push_pop_fifo_order():
    buf: RingBuffer[int] = RingBuffer(capacity=4)
    buf.push(1)
    buf.push(2)
    buf.push(3)

    async def drain():
        return [await buf.pop(), await buf.pop(), await buf.pop()]

    assert asyncio.run(drain()) == [1, 2, 3]


def test_overflow_drops_oldest():
    buf: RingBuffer[int] = RingBuffer(capacity=2)
    buf.push(1)
    buf.push(2)
    buf.push(3)  # should drop 1

    assert buf.dropped == 1
    assert len(buf) == 2

    async def drain():
        return [await buf.pop(), await buf.pop()]

    assert asyncio.run(drain()) == [2, 3]


def test_pop_waits_for_item():
    buf: RingBuffer[int] = RingBuffer(capacity=2)

    async def scenario():
        pop_task = asyncio.create_task(buf.pop())
        await asyncio.sleep(0.01)
        assert not pop_task.done()
        buf.push(42)
        result = await asyncio.wait_for(pop_task, timeout=1)
        assert result == 42

    asyncio.run(scenario())


def test_zero_capacity_rejected():
    with pytest.raises(ValueError):
        RingBuffer(capacity=0)
