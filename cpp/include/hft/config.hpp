// Configuration loading.
//
// Hardcoded constants are fine for a demo and unacceptable for anything you
// intend to operate: limits, strategy parameters and venue costs are exactly
// the things you need to change without a rebuild, and exactly the things you
// need a record of when explaining what the engine was doing at 09:31.
//
// Format is deliberately boring `key = value`, `#` comments, no sections, no
// dependencies. Unknown keys are an ERROR, not a warning: a typo'd
// `max_postion_per_symbol` that silently leaves the real limit at its default
// is precisely the failure this file exists to prevent.
#pragma once

#include <string>
#include <vector>

#include "hft/engine.hpp"
#include "hft/log.hpp"

namespace hft {

// Everything the application needs, in one place.
struct AppConfig {
  EngineConfig engine;

  // Feed
  std::string symbol = "SYNTH";
  std::size_t events = 2'000'000;
  std::uint64_t seed = 0x5EEDC0DEULL;
  std::string replay_path;  // when set, replay this CSV instead of generating

  // Output
  std::string out_dir;
  std::size_t depth_levels = 15;
  LogLevel log_level = LogLevel::Info;

  // Shutdown behaviour
  bool flatten_on_exit = true;
};

struct ConfigError {
  std::size_t line = 0;
  std::string message;
};

// Loads `path` over the top of `cfg` (so unspecified keys keep their
// defaults). Returns false and fills `errors` if anything is wrong; on failure
// `cfg` may be partially modified, so callers should load into a fresh copy.
bool load_config_file(const std::string& path, AppConfig& cfg, std::vector<ConfigError>& errors);

// Applies a single `key=value` pair. Exposed so the CLI can use exactly the
// same parsing and validation as the file, rather than a second code path that
// drifts. Returns false with a reason in `error`.
bool apply_config_setting(const std::string& key, const std::string& value, AppConfig& cfg,
                          std::string& error);

// Every recognised key, for `--help` and for validating docs against code.
const std::vector<std::string>& config_keys();

// Renders the effective configuration, so a run's log records exactly what it
// was configured with.
std::string describe_config(const AppConfig& cfg);

// Cross-field validation (e.g. fast window must be below slow window). Called
// after all sources are merged.
bool validate_config(const AppConfig& cfg, std::vector<ConfigError>& errors);

}  // namespace hft
