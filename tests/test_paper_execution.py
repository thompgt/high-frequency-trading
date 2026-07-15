import asyncio

from hft.execution.base import Order
from hft.execution.paper import PaperExecutionVenue


def test_buy_opens_long_position():
    venue = PaperExecutionVenue()
    order = Order(symbol="AAPL", side="BUY", quantity=10)
    fill = asyncio.run(venue.submit(order, reference_price=100.0))

    assert fill.price == 100.0
    assert venue.positions["AAPL"] == 10


def test_sell_after_buy_realizes_pnl():
    venue = PaperExecutionVenue()
    asyncio.run(venue.submit(Order(symbol="AAPL", side="BUY", quantity=10), reference_price=100.0))
    asyncio.run(venue.submit(Order(symbol="AAPL", side="SELL", quantity=10), reference_price=110.0))

    assert venue.positions["AAPL"] == 0
    assert venue.realized_pnl == 100.0  # 10 * (110 - 100)


def test_slippage_worsens_fill_price():
    venue = PaperExecutionVenue(slippage_bps=100.0)  # 1%
    fill = asyncio.run(
        venue.submit(Order(symbol="AAPL", side="BUY", quantity=1), reference_price=100.0)
    )
    assert fill.price == 101.0


def test_fees_reduce_realized_pnl():
    venue = PaperExecutionVenue(fee_bps=100.0)  # 1% fee each side
    asyncio.run(venue.submit(Order(symbol="AAPL", side="BUY", quantity=10), reference_price=100.0))
    asyncio.run(venue.submit(Order(symbol="AAPL", side="SELL", quantity=10), reference_price=100.0))

    # no price movement, so PnL should be purely negative fees
    assert venue.realized_pnl < 0
