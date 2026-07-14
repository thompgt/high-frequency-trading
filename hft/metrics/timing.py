"""Nanosecond stage-boundary latency recording. You can't optimize what you
don't measure -- every tick's ingest->signal and (when a signal fires)
signal->order durations are tracked in-memory and can be dumped to CSV for
percentile analysis, without doing any network I/O on the hot path."""

from __future__ import annotations

import csv
import logging
import statistics
from dataclasses import dataclass
from pathlib import Path

logger = logging.getLogger(__name__)


@dataclass(frozen=True, slots=True)
class LatencySample:
    ingest_to_signal_ns: int
    signal_compute_ns: int
    order_round_trip_ns: int | None  # None when no signal/order fired


class LatencyRecorder:
    def __init__(self, max_samples: int = 100_000) -> None:
        self.max_samples = max_samples
        self._samples: list[LatencySample] = []

    def record_tick(self, tick_ingest_ns: int, signal_start_ns: int, signal_end_ns: int) -> None:
        self._append(
            LatencySample(
                ingest_to_signal_ns=signal_start_ns - tick_ingest_ns,
                signal_compute_ns=signal_end_ns - signal_start_ns,
                order_round_trip_ns=None,
            )
        )

    def record_signal(
        self,
        tick_ingest_ns: int,
        signal_start_ns: int,
        signal_end_ns: int,
        order_start_ns: int,
        order_end_ns: int,
    ) -> None:
        self._append(
            LatencySample(
                ingest_to_signal_ns=signal_start_ns - tick_ingest_ns,
                signal_compute_ns=signal_end_ns - signal_start_ns,
                order_round_trip_ns=order_end_ns - order_start_ns,
            )
        )

    def _append(self, sample: LatencySample) -> None:
        self._samples.append(sample)
        if len(self._samples) > self.max_samples:
            self._samples.pop(0)

    def summary(self) -> dict[str, float]:
        if not self._samples:
            return {}
        ingest_to_signal = [s.ingest_to_signal_ns for s in self._samples]
        order_rt = [s.order_round_trip_ns for s in self._samples if s.order_round_trip_ns is not None]

        out = {
            "count": len(self._samples),
            "ingest_to_signal_ns_p50": _percentile(ingest_to_signal, 50),
            "ingest_to_signal_ns_p99": _percentile(ingest_to_signal, 99),
        }
        if order_rt:
            out["order_round_trip_ns_p50"] = _percentile(order_rt, 50)
            out["order_round_trip_ns_p99"] = _percentile(order_rt, 99)
        return out

    def log_summary(self) -> None:
        summary = self.summary()
        if summary:
            logger.info("latency summary: %s", summary)

    def dump_csv(self, path: Path) -> None:
        with path.open("w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["ingest_to_signal_ns", "signal_compute_ns", "order_round_trip_ns"])
            for s in self._samples:
                writer.writerow([s.ingest_to_signal_ns, s.signal_compute_ns, s.order_round_trip_ns])


def _percentile(values: list[int], pct: float) -> float:
    if len(values) == 1:
        return float(values[0])
    return statistics.quantiles(values, n=100, method="inclusive")[int(pct) - 1]
