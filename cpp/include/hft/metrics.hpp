// Metrics export.
//
// A run that leaves nothing behind cannot be debugged after the fact. On exit
// the engine writes a single JSON document with everything you would want at
// 09:32 when asked what happened at 09:31: throughput, fills, PnL, the full
// reject breakdown by reason, the halt state, and latency percentiles for
// every stage.
//
// JSON is hand-rolled rather than pulled from a library: it is ~100 lines for
// a fixed, known schema, and adding a dependency to an engine that currently
// needs nothing but a compiler is a bad trade.
#pragma once

#include <string>

#include "hft/engine.hpp"
#include "hft/symbol_table.hpp"

namespace hft {

// Writes the full run report. Returns false if the file could not be written
// (a metrics write failing must be reported, not swallowed).
bool write_metrics_json(const std::string& path, const Engine& engine, const EngineStats& stats,
                        const SymbolTable& symbols);

// The same document as a string, for logging or tests.
std::string metrics_json(const Engine& engine, const EngineStats& stats,
                         const SymbolTable& symbols);

}  // namespace hft
