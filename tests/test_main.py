"""Entrypoint argument and environment handling.

main.py is where the CLI, the environment and the defaults in Config are
reconciled, and it is the layer the Docker image drives -- the container sets
HFT_METRICS rather than rewriting the command line. The precedence rules
between those three sources were untested, which is unfortunate for the one
piece of code whose only consumer is a deployment.
"""

from __future__ import annotations

import pytest

from hft.config import Config
from hft.main import _env_flag, parse_args


def test_no_arguments_leaves_the_defaults_alone():
    cfg = parse_args([])
    default = Config()
    assert cfg.symbols == default.symbols
    assert cfg.poll_interval_s == default.poll_interval_s
    assert cfg.fast_window == default.fast_window
    assert cfg.slow_window == default.slow_window


def test_arguments_override_the_defaults():
    cfg = parse_args(
        [
            "--symbols", "AAPL", "MSFT",
            "--poll-interval", "2.5",
            "--fast-window", "3",
            "--slow-window", "9",
            "--summary-interval", "30",
            "--latency-csv", "out/latency.csv",
        ]
    )
    assert cfg.symbols == ["AAPL", "MSFT"]
    assert cfg.poll_interval_s == 2.5
    assert cfg.fast_window == 3
    assert cfg.slow_window == 9
    assert cfg.summary_interval_s == 30
    assert cfg.latency_csv_path == "out/latency.csv"


def test_metrics_are_off_unless_asked_for(monkeypatch):
    monkeypatch.delenv("HFT_METRICS", raising=False)
    assert parse_args([]).metrics_enabled is False
    assert parse_args(["--metrics"]).metrics_enabled is True


@pytest.mark.parametrize("value", ["1", "true", "TRUE", "yes", "on", " on "])
def test_the_env_var_turns_metrics_on(monkeypatch, value):
    monkeypatch.setenv("HFT_METRICS", value)
    assert _env_flag("HFT_METRICS") is True
    assert parse_args([]).metrics_enabled is True


@pytest.mark.parametrize("value", ["", "0", "false", "no", "off", "maybe"])
def test_anything_else_leaves_metrics_off(monkeypatch, value):
    monkeypatch.setenv("HFT_METRICS", value)
    assert _env_flag("HFT_METRICS") is False
    assert parse_args([]).metrics_enabled is False


def test_the_flag_beats_the_env_var(monkeypatch):
    # The container sets the env var; someone running the image with an
    # explicit flag is overriding it on purpose.
    monkeypatch.setenv("HFT_METRICS", "0")
    assert parse_args(["--metrics"]).metrics_enabled is True


def test_metrics_port_comes_from_the_flag_then_the_env_then_the_default(monkeypatch):
    monkeypatch.delenv("HFT_METRICS_PORT", raising=False)
    assert parse_args([]).metrics_port == Config().metrics_port

    monkeypatch.setenv("HFT_METRICS_PORT", "9999")
    assert parse_args([]).metrics_port == 9999
    assert parse_args(["--metrics-port", "9200"]).metrics_port == 9200
