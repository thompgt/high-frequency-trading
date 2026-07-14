from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(slots=True)
class Config:
    symbols: list[str] = field(default_factory=lambda: ["AAPL", "MSFT"])
    poll_interval_s: float = 1.0
    buffer_capacity: int = 1024
    fast_window: int = 5
    slow_window: int = 20
    order_quantity: int = 1
    slippage_bps: float = 1.0
    fee_bps: float = 0.5
    summary_interval_s: float = 30.0
    latency_csv_path: str | None = None
