"""Bounded single-producer/single-consumer buffer between ingestion and the
strategy engine.

This is an asyncio-native SPSC ring buffer: a fixed-size preallocated list
plus head/tail indices, so steady-state operation does no allocation. It
intentionally drops the oldest item on overflow rather than blocking the
producer -- in a trading pipeline, stalling ingestion to avoid dropping a
stale tick is the wrong tradeoff.

Note: this runs within a single process/event loop. If the data source and
engine are later split across processes for true isolation, replace this
with a shared-memory ring buffer (e.g. multiprocessing.shared_memory) behind
the same push/pop interface.
"""

from __future__ import annotations

import asyncio
from typing import Generic, TypeVar

T = TypeVar("T")


class RingBuffer(Generic[T]):
    def __init__(self, capacity: int) -> None:
        if capacity <= 0:
            raise ValueError("capacity must be positive")
        self._capacity = capacity
        self._buf: list[T | None] = [None] * capacity
        self._head = 0  # next write index
        self._tail = 0  # next read index
        self._size = 0
        self._dropped = 0
        self._not_empty = asyncio.Event()

    @property
    def dropped(self) -> int:
        return self._dropped

    def push(self, item: T) -> None:
        if self._size == self._capacity:
            # overflow: drop the oldest item to make room for the newest
            self._tail = (self._tail + 1) % self._capacity
            self._size -= 1
            self._dropped += 1
        self._buf[self._head] = item
        self._head = (self._head + 1) % self._capacity
        self._size += 1
        self._not_empty.set()

    async def pop(self) -> T:
        while self._size == 0:
            await self._not_empty.wait()
        item = self._buf[self._tail]
        self._buf[self._tail] = None
        self._tail = (self._tail + 1) % self._capacity
        self._size -= 1
        if self._size == 0:
            self._not_empty.clear()
        assert item is not None
        return item

    def __len__(self) -> int:
        return self._size
