"""Tests mock yfinance entirely -- no live network calls."""

import asyncio
from unittest.mock import MagicMock

from hft.data.yfinance_source import YFinanceSource


def test_fetch_one_builds_tick_from_fast_info():
    ticker = MagicMock()
    ticker.fast_info = {"lastPrice": 123.45, "lastVolume": 500}

    tick = YFinanceSource._fetch_one("AAPL", ticker)

    assert tick is not None
    assert tick.symbol == "AAPL"
    assert tick.price == 123.45
    assert tick.volume == 500


def test_fetch_one_returns_none_without_price():
    ticker = MagicMock()
    ticker.fast_info = {"lastPrice": None}

    assert YFinanceSource._fetch_one("AAPL", ticker) is None


def test_stream_yields_ticks_for_each_symbol(monkeypatch):
    fake_tickers = {
        "AAPL": MagicMock(fast_info={"lastPrice": 100.0, "lastVolume": 10}),
        "MSFT": MagicMock(fast_info={"lastPrice": 200.0, "lastVolume": 20}),
    }
    monkeypatch.setattr(
        "hft.data.yfinance_source.yf.Ticker", lambda sym: fake_tickers[sym]
    )

    source = YFinanceSource(poll_interval_s=0.01)

    async def scenario():
        collected = []
        async for tick in source.stream(list(fake_tickers.keys())):
            collected.append(tick)
            if len(collected) == 2:
                break
        return collected

    ticks = asyncio.run(scenario())
    assert {t.symbol for t in ticks} == {"AAPL", "MSFT"}
