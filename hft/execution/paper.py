"""Paper-trading execution venue: fills instantly at the reference price
(optionally with simple slippage/fee modeling) and tracks position + PnL
in memory. This is the default venue until a real broker integration is
wired up behind the same ExecutionVenue interface."""

from __future__ import annotations

from hft.execution.base import ExecutionVenue, Fill, Order


class PaperExecutionVenue(ExecutionVenue):
    def __init__(self, slippage_bps: float = 0.0, fee_bps: float = 0.0) -> None:
        self.slippage_bps = slippage_bps
        self.fee_bps = fee_bps
        self.positions: dict[str, int] = {}
        self.realized_pnl: float = 0.0
        self._cost_basis: dict[str, float] = {}

    async def submit(self, order: Order, reference_price: float) -> Fill:
        slip = reference_price * (self.slippage_bps / 10_000)
        fill_price = reference_price + slip if order.side == "BUY" else reference_price - slip
        fee = fill_price * order.quantity * (self.fee_bps / 10_000)

        signed_qty = order.quantity if order.side == "BUY" else -order.quantity
        prev_qty = self.positions.get(order.symbol, 0)
        prev_cost = self._cost_basis.get(order.symbol, 0.0)

        if prev_qty == 0 or (prev_qty > 0) == (signed_qty > 0):
            # adding to (or opening) a position
            new_qty = prev_qty + signed_qty
            self._cost_basis[order.symbol] = prev_cost + fill_price * signed_qty
        else:
            # reducing or flipping a position: realize PnL on the closed portion
            closing_qty = min(abs(signed_qty), abs(prev_qty))
            avg_entry = prev_cost / prev_qty if prev_qty else 0.0
            direction = 1 if prev_qty > 0 else -1
            self.realized_pnl += (fill_price - avg_entry) * closing_qty * direction
            new_qty = prev_qty + signed_qty
            self._cost_basis[order.symbol] = avg_entry * new_qty if new_qty else 0.0

        self.realized_pnl -= fee
        self.positions[order.symbol] = new_qty

        return Fill(symbol=order.symbol, side=order.side, quantity=order.quantity, price=fill_price)
