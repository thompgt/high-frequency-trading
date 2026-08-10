"""Integer minor units, mirroring ``cpp/include/hft/types.hpp``.

The C++ engine represents every price as an ``std::int64_t`` count of ticks --
cents, by default -- because a price is a count of the smallest thing the
venue will quote, not a measurement. The Python pipeline grew up around
``float`` dollars, which is a different model: 0.1 + 0.2 is not 0.3, a fill
price can land between two quotable prices, and two "equivalent"
implementations that disagree in the seventh decimal cannot be diffed against
each other at all.

This module is the bridge. The conversions are deliberately the same
arithmetic as ``price_from_double`` / ``price_to_double``, rounding half away
from zero rather than to even, so a dollar value crossing the boundary lands
on the same tick on both sides.

What this does *not* do is turn the Python pipeline into an integer-tick
engine end to end: ``Tick.price`` is still the float yfinance reports, and the
strategy still averages floats. What it does is put the venue -- the one place
that produces a price someone would settle against -- on the same grid as the
C++ venue, so their fills agree to the cent and a cross-implementation
comparison is meaningful. See the divergence note in the README.
"""

from __future__ import annotations

#: Ticks per unit of currency. 12345 ticks == $123.45. Must match kTickScale.
TICK_SCALE = 100


def to_ticks(price: float) -> int:
    """Dollars -> integer minor units, rounding half away from zero."""
    scaled = price * TICK_SCALE
    return int(scaled + (0.5 if scaled >= 0 else -0.5))


def from_ticks(ticks: int) -> float:
    """Integer minor units -> dollars."""
    return ticks / TICK_SCALE


def quantize(price: float) -> float:
    """Snap a dollar value onto the tick grid the venue can actually quote."""
    return from_ticks(to_ticks(price))


__all__ = ["TICK_SCALE", "to_ticks", "from_ticks", "quantize"]
