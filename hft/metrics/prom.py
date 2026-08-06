"""Live Prometheus surface for the Python pipeline.

The C++ engine writes an end-of-run JSON report (``cpp/src/metrics.cpp``),
which is fine for benchmarking but useless for watching a session as it
happens. This module is the live counterpart: an in-process registry that
the ingest loop, strategy engine and paper venue update on the hot path,
exposed over HTTP for Prometheus to scrape (see ``monitoring/``).

Two design rules worth keeping:

* **Low label cardinality.** Labels are symbol / side / reason only -- all
  small, bounded sets. Never label by order id, timestamp or price: each
  distinct label combination is a separate time series in Prometheus, and
  per-order labels turn a 20-series exporter into a memory leak.
* **No duplicated timing logic.** Latency histograms are fed from the
  existing ``LatencyRecorder`` samples (``hft/metrics/timing.py``) via
  :func:`observe_latency_sample`, so there is exactly one place that decides
  what "ingest to signal" means.

Recording is always safe to call -- the counters live in a plain registry
whether or not anything is scraping them. Only the HTTP listener is behind a
flag (:func:`start_exporter`), so tests and CI never bind a port.
"""

from __future__ import annotations

import logging
from typing import TYPE_CHECKING

from prometheus_client import CollectorRegistry, Counter, Gauge, Histogram, start_http_server

if TYPE_CHECKING:  # pragma: no cover - typing only
    from hft.metrics.timing import LatencySample

logger = logging.getLogger(__name__)

#: Default exporter port. 9101 sits in the "unassigned exporter" range and
#: avoids Prometheus' own 9090 and Grafana's 3000.
DEFAULT_PORT = 9101

#: Dedicated registry rather than prometheus_client.REGISTRY, so ``/metrics``
#: carries only pipeline series -- no interpreter/GC noise to scroll past.
REGISTRY = CollectorRegistry()

# Stage latencies are microsecond-scale in-process, but a stalled yfinance
# call or a GC pause can push the tail into milliseconds; the buckets span
# 1us -> 1s so the p99 panel does not saturate in the top bucket.
_LATENCY_BUCKETS = (
    1e-6,
    5e-6,
    1e-5,
    5e-5,
    1e-4,
    5e-4,
    1e-3,
    5e-3,
    1e-2,
    5e-2,
    0.1,
    0.5,
    1.0,
)

# --- ingestion ------------------------------------------------------------

TICKS_INGESTED = Counter(
    "hft_ticks_ingested_total",
    "Market-data ticks pushed into the ring buffer.",
    ["symbol"],
    registry=REGISTRY,
)

TICKS_DROPPED = Counter(
    "hft_ticks_dropped_total",
    "Ticks discarded by the ring buffer on overflow (oldest-first).",
    registry=REGISTRY,
)

BUFFER_DEPTH = Gauge(
    "hft_buffer_depth",
    "Ticks currently queued in the ring buffer, awaiting the strategy engine.",
    registry=REGISTRY,
)

FEED_ERRORS = Counter(
    "hft_feed_errors_total",
    "Market-data poll failures, labelled by exception type.",
    ["reason"],
    registry=REGISTRY,
)

# --- strategy -------------------------------------------------------------

SIGNALS = Counter(
    "hft_signals_total",
    "Signals emitted by the strategy, labelled by side.",
    ["side"],
    registry=REGISTRY,
)

# --- execution ------------------------------------------------------------

ORDERS_SUBMITTED = Counter(
    "hft_orders_submitted_total",
    "Orders handed to the execution venue.",
    ["side"],
    registry=REGISTRY,
)

ORDERS_FILLED = Counter(
    "hft_orders_filled_total",
    "Orders the venue reported filled.",
    ["side"],
    registry=REGISTRY,
)

ORDERS_REJECTED = Counter(
    "hft_orders_rejected_total",
    "Orders the venue refused or failed to execute, labelled by reason.",
    ["reason"],
    registry=REGISTRY,
)

FILLED_QUANTITY = Counter(
    "hft_filled_quantity_total",
    "Total filled quantity, labelled by side.",
    ["side"],
    registry=REGISTRY,
)

# --- position / P&L -------------------------------------------------------

POSITION = Gauge(
    "hft_position_quantity",
    "Signed open position per symbol (positive = long).",
    ["symbol"],
    registry=REGISTRY,
)

REALIZED_PNL = Gauge(
    "hft_realized_pnl",
    "Cumulative realized P&L of the paper venue, net of modelled fees.",
    registry=REGISTRY,
)

LAST_PRICE = Gauge(
    "hft_last_price",
    "Most recent ingested price per symbol.",
    ["symbol"],
    registry=REGISTRY,
)

# --- latency --------------------------------------------------------------

INGEST_TO_SIGNAL = Histogram(
    "hft_ingest_to_signal_seconds",
    "Tick ingest timestamp -> start of strategy evaluation.",
    buckets=_LATENCY_BUCKETS,
    registry=REGISTRY,
)

SIGNAL_COMPUTE = Histogram(
    "hft_signal_compute_seconds",
    "Time spent inside Strategy.on_tick.",
    buckets=_LATENCY_BUCKETS,
    registry=REGISTRY,
)

ORDER_ROUND_TRIP = Histogram(
    "hft_order_round_trip_seconds",
    "Order submit -> fill returned by the execution venue.",
    buckets=_LATENCY_BUCKETS,
    registry=REGISTRY,
)

_NS_PER_S = 1e9

_server_started = False


# --- recording helpers ----------------------------------------------------


def observe_latency_sample(sample: "LatencySample") -> None:
    """Feed one :class:`~hft.metrics.timing.LatencySample` into the histograms.

    Wired in as ``LatencyRecorder(on_sample=observe_latency_sample)`` so the
    recorder stays the single source of truth for stage boundaries and this
    module only converts ns -> seconds (Prometheus convention is base units).
    """
    INGEST_TO_SIGNAL.observe(sample.ingest_to_signal_ns / _NS_PER_S)
    SIGNAL_COMPUTE.observe(sample.signal_compute_ns / _NS_PER_S)
    if sample.order_round_trip_ns is not None:
        ORDER_ROUND_TRIP.observe(sample.order_round_trip_ns / _NS_PER_S)


def record_tick(symbol: str, price: float) -> None:
    TICKS_INGESTED.labels(symbol=symbol).inc()
    LAST_PRICE.labels(symbol=symbol).set(price)


def record_buffer_state(depth: int, dropped_total: int) -> None:
    """Sync the buffer gauges.

    ``RingBuffer`` owns ``dropped`` as a monotonic count, so the delta since
    the last call is what the counter needs -- calling ``.inc(total)`` would
    compound.
    """
    BUFFER_DEPTH.set(depth)
    delta = dropped_total - record_buffer_state._last_dropped  # type: ignore[attr-defined]
    if delta > 0:
        TICKS_DROPPED.inc(delta)
    record_buffer_state._last_dropped = dropped_total  # type: ignore[attr-defined]


record_buffer_state._last_dropped = 0  # type: ignore[attr-defined]


def record_feed_error(exc: BaseException) -> None:
    FEED_ERRORS.labels(reason=type(exc).__name__).inc()


def record_signal(side: str) -> None:
    SIGNALS.labels(side=side).inc()


def record_order_submitted(side: str) -> None:
    ORDERS_SUBMITTED.labels(side=side).inc()


def record_order_filled(side: str, quantity: int) -> None:
    ORDERS_FILLED.labels(side=side).inc()
    FILLED_QUANTITY.labels(side=side).inc(quantity)


def record_order_rejected(reason: str) -> None:
    """``reason`` must come from a bounded set (exception type names), never
    a formatted message containing an id, symbol-price or timestamp."""
    ORDERS_REJECTED.labels(reason=reason).inc()


def record_venue_state(positions: dict[str, int], realized_pnl: float) -> None:
    for symbol, quantity in positions.items():
        POSITION.labels(symbol=symbol).set(quantity)
    REALIZED_PNL.set(realized_pnl)


# --- exporter -------------------------------------------------------------


def start_exporter(port: int = DEFAULT_PORT, addr: str = "0.0.0.0") -> bool:
    """Bind the ``/metrics`` HTTP listener. Returns True if it is now serving.

    Idempotent, and never fatal: a metrics port that is already taken should
    not stop a trading session from running, so a bind failure is logged and
    the pipeline continues with recording-only metrics.
    """
    global _server_started
    if _server_started:
        return True
    try:
        start_http_server(port, addr=addr, registry=REGISTRY)
    except OSError:
        logger.exception("could not bind metrics exporter on %s:%d; continuing without it", addr, port)
        return False
    _server_started = True
    logger.info("prometheus exporter listening on http://localhost:%d/metrics", port)
    return True
