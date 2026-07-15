import time

from hft.core.strategy import MovingAverageCrossoverStrategy
from hft.data.base import Tick


def make_tick(symbol: str, price: float) -> Tick:
    return Tick(symbol=symbol, price=price, volume=100, source_ts_ns=time.time_ns())


def test_no_signal_until_slow_window_filled():
    strat = MovingAverageCrossoverStrategy(fast_window=2, slow_window=4)
    for price in [100, 101, 102]:
        assert strat.on_tick(make_tick("AAPL", price)) is None


def test_detects_bullish_crossover():
    strat = MovingAverageCrossoverStrategy(fast_window=2, slow_window=4)
    # Build a slow-declining-then-rising series so fast crosses above slow.
    prices = [100, 99, 98, 97, 105, 110]
    signals = [strat.on_tick(make_tick("AAPL", p)) for p in prices]
    fired = [s for s in signals if s is not None]
    assert any(s.side == "BUY" for s in fired)


def test_symbols_tracked_independently():
    strat = MovingAverageCrossoverStrategy(fast_window=2, slow_window=3)
    for p in [100, 100, 100]:
        strat.on_tick(make_tick("AAPL", p))
    # MSFT has its own history and shouldn't have enough data for a signal yet.
    assert strat.on_tick(make_tick("MSFT", 50)) is None


def test_rejects_invalid_windows():
    import pytest

    with pytest.raises(ValueError):
        MovingAverageCrossoverStrategy(fast_window=10, slow_window=5)
