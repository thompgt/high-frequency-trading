"""The Python pipeline works in float dollars; the C++ engine works in integer
ticks. The README claims the two produce comparable results, which is only
true if a price means the same thing on both sides.

The reference implementation is cpp/include/hft/types.hpp:

    inline constexpr std::int64_t kTickScale = 100;

    inline constexpr Price price_from_double(double px) {
      return static_cast<Price>(px * kTickScale + (px >= 0 ? 0.5 : -0.5));
    }

    inline constexpr double price_to_double(Price px) {
      return static_cast<double>(px) / static_cast<double>(kTickScale);
    }

These tests pin hft/money.py to that arithmetic -- including the rounding
rule, which is half *away from zero* and not Python's default round-half-even.
"""

from __future__ import annotations

import asyncio
import re
from pathlib import Path

import pytest

from hft.execution.base import Order
from hft.execution.paper import PaperExecutionVenue
from hft.money import TICK_SCALE, from_ticks, quantize, to_ticks

TYPES_HPP = Path(__file__).resolve().parents[1] / "cpp" / "include" / "hft" / "types.hpp"


def test_tick_scale_matches_the_cpp_engine():
    source = TYPES_HPP.read_text(encoding="utf-8")
    match = re.search(r"kTickScale\s*=\s*(\d+)", source)
    assert match, "kTickScale not found in types.hpp -- the constant moved"
    assert TICK_SCALE == int(match.group(1))


@pytest.mark.parametrize(
    "dollars,ticks",
    [
        (0.0, 0),
        (1.0, 100),
        (123.45, 12345),
        (0.01, 1),
        (-123.45, -12345),
        (99.999, 10000),
    ],
)
def test_conversion_matches_price_from_double(dollars, ticks):
    assert to_ticks(dollars) == ticks


def test_rounding_is_half_away_from_zero_not_half_to_even():
    # Python's round() would give 0 and 2 here; the C++ rule gives 1 and 2.
    assert to_ticks(0.005) == 1
    assert to_ticks(0.015) == 2
    assert to_ticks(-0.005) == -1


def test_round_trip_is_exact_on_the_grid():
    for ticks in range(-500, 500):
        assert to_ticks(from_ticks(ticks)) == ticks


def test_quantize_snaps_to_a_quotable_price():
    assert quantize(100.004) == 100.0
    assert quantize(100.006) == 100.01


def test_paper_venue_fills_land_on_the_tick_grid():
    # 33 bps on $100 is $0.33 exactly; 7 bps is $0.07 after rounding, not
    # $0.06999999999999999.
    venue = PaperExecutionVenue(slippage_bps=7.0)
    fill = asyncio.run(venue.submit(Order("AAPL", "BUY", 1), reference_price=100.0))
    assert fill.price == 100.07
    assert to_ticks(fill.price) == 10007

    # And the sell side is the mirror image, on the same grid.
    venue = PaperExecutionVenue(slippage_bps=7.0)
    fill = asyncio.run(venue.submit(Order("AAPL", "SELL", 1), reference_price=100.0))
    assert fill.price == 99.93


def test_a_fill_price_is_never_between_two_quotable_prices():
    venue = PaperExecutionVenue(slippage_bps=13.0)
    for reference in (1.007, 19.995, 191.2349, 1234.5678):
        fill = asyncio.run(venue.submit(Order("AAPL", "BUY", 1), reference_price=reference))
        assert from_ticks(to_ticks(fill.price)) == fill.price
