"""Every timestamp that gets subtracted from another must come off the
monotonic clock, not the wall clock. This is easy to regress by writing the
obvious `time.time_ns()` in a new dataclass default, so the check is on the
clock *domain* rather than on the words in the source."""

import time

from hft.clock import monotonic_ns
from hft.data.base import Tick
from hft.execution.base import Fill, Order

# The two clocks have unrelated epochs: the wall clock counts from 1970, the
# performance counter from an arbitrary point (boot, process start, ...).
# A value from one is nowhere near a value from the other.
ONE_HOUR_NS = 3_600_000_000_000


def _is_monotonic_domain(value: int) -> bool:
    return abs(value - monotonic_ns()) < ONE_HOUR_NS


def test_monotonic_clock_is_not_the_wall_clock():
    assert monotonic_ns() is not None
    assert not _is_monotonic_domain(time.time_ns())


def test_latency_timestamps_come_off_the_monotonic_clock():
    # A wall-clock value here would make every recorded duration a mixture of
    # elapsed time and whatever NTP did during the sample window.
    assert _is_monotonic_domain(Tick("AAPL", 100.0, 1, source_ts_ns=0).ingest_ts_ns)
    assert _is_monotonic_domain(Order("AAPL", "BUY", 1).created_ts_ns)
    assert _is_monotonic_domain(Fill("AAPL", "BUY", 1, 100.0).filled_ts_ns)


def test_monotonic_clock_never_goes_backwards():
    last = monotonic_ns()
    for _ in range(1000):
        now = monotonic_ns()
        assert now >= last
        last = now


def test_source_timestamp_stays_on_the_wall_clock():
    # source_ts_ns names a moment in the outside world -- "when the venue says
    # this happened" -- so it is deliberately not monotonic, and is never
    # differenced against the monotonic stamps.
    tick = Tick("AAPL", 100.0, 1, source_ts_ns=time.time_ns())
    assert not _is_monotonic_domain(tick.source_ts_ns)
