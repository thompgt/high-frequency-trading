"""The one clock every latency measurement in the pipeline reads.

``time.time_ns()`` is a wall clock. It is adjustable, and NTP adjusts it: the
value can jump forwards, and -- on a step correction, or when a VM's clock is
resynchronised after a suspend -- backwards. Subtracting two of its readings
is therefore not a duration. Do it on a hot path and the p99 you report is a
mixture of the code you are measuring and whatever the time daemon did during
the sample window, including negative "latencies" that quietly poison every
percentile computed from them.

``time.perf_counter_ns()`` is monotonic by contract, has the highest
resolution the platform offers, and is exactly what the C++ side documents
for ``std::chrono::steady_clock``. The epoch is arbitrary and unrelated to
the wall clock, which is the whole point: these values are only ever
subtracted from each other, never displayed as a time of day.

The one thing this clock is *not* for is a timestamp that names a moment --
"when did the exchange say this trade happened". That is a wall-clock fact
about the outside world, so ``Tick.source_ts_ns`` stays on ``time.time_ns()``
and is never differenced against anything measured here.
"""

from __future__ import annotations

import time

# Bound once rather than looked up per call: this runs per tick.
monotonic_ns = time.perf_counter_ns

__all__ = ["monotonic_ns"]
