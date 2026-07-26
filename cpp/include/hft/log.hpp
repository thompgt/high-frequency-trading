// Asynchronous structured logging.
//
// The rule this exists to enforce: **no formatting and no I/O on the tick
// path**. A single fprintf inside the hot loop costs microseconds and, worse,
// is unbounded -- a blocked stdout stalls trading. Both are unacceptable in an
// engine whose whole point is nanosecond-scale latency.
//
// So the producer side does the cheapest possible thing: stamp a clock, copy a
// few scalars and a couple of pointers into a preallocated slot, publish. A
// background thread does all the formatting and writing. The record is a POD
// carrying only pointers to *string literals* (static lifetime, never freed)
// plus numeric fields -- no strings are copied, nothing is allocated.
//
// When the queue is full the record is dropped and counted rather than
// blocking. Losing a log line is always better than delaying an order; the
// drop count is reported at shutdown so the loss is never silent.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "hft/latency.hpp"
#include "hft/ring_buffer.hpp"
#include "hft/types.hpp"

namespace hft {

enum class LogLevel : std::uint8_t { Trace = 0, Debug, Info, Warn, Error, Off };

const char* to_string(LogLevel l);
// Parses "trace"/"debug"/"info"/"warn"/"error"/"off"; returns false if unknown.
bool parse_log_level(const std::string& text, LogLevel& out);

// One log event. Trivially copyable so it can go straight through the SPSC
// ring. `message` and the `key` pointers MUST have static storage duration --
// in practice they are always string literals at the call site.
struct LogRecord {
  Nanos ts_ns = 0;
  const char* message = nullptr;
  const char* key[3] = {nullptr, nullptr, nullptr};
  double value[3] = {0.0, 0.0, 0.0};
  LogLevel level = LogLevel::Info;
  std::uint8_t field_count = 0;
};

class Logger {
 public:
  // `out` is not owned. Defaults to stderr so log output never contaminates a
  // stdout data stream.
  explicit Logger(LogLevel min_level = LogLevel::Info, std::FILE* out = nullptr,
                  std::size_t capacity = 1 << 14);
  ~Logger();

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  void start();
  // Drains everything still queued, then joins the writer thread.
  void stop();

  void set_level(LogLevel l) { min_level_.store(l, std::memory_order_relaxed); }
  LogLevel level() const { return min_level_.load(std::memory_order_relaxed); }
  bool enabled(LogLevel l) const { return l >= level() && l != LogLevel::Off; }

  // Hot-path entry points. Each is a level test, a clock read, a struct fill
  // and a ring push -- no formatting, no allocation, no locks.
  void log(LogLevel l, const char* message);
  void log(LogLevel l, const char* message, const char* k0, double v0);
  void log(LogLevel l, const char* message, const char* k0, double v0, const char* k1, double v1);
  void log(LogLevel l, const char* message, const char* k0, double v0, const char* k1, double v1,
           const char* k2, double v2);

  std::uint64_t dropped() const { return queue_.dropped(); }
  std::uint64_t written() const { return written_.load(std::memory_order_relaxed); }

 private:
  void emit(const LogRecord& rec);
  void run();
  void write_one(const LogRecord& rec);

  std::atomic<LogLevel> min_level_;
  std::FILE* out_;
  SpscRingBuffer<LogRecord> queue_;
  std::thread writer_;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> written_{0};
};

// The process-wide logger. A singleton is the right shape here: the whole
// point is that there is exactly one writer thread and one output stream.
Logger& default_logger();

}  // namespace hft

// Level check happens before anything else is evaluated, so a disabled log
// statement costs one relaxed atomic load.
#define HFT_LOG(level_, ...)                                       \
  do {                                                             \
    ::hft::Logger& lg_ = ::hft::default_logger();                  \
    if (lg_.enabled(level_)) lg_.log(level_, __VA_ARGS__);         \
  } while (0)

#define HFT_INFO(...) HFT_LOG(::hft::LogLevel::Info, __VA_ARGS__)
#define HFT_WARN(...) HFT_LOG(::hft::LogLevel::Warn, __VA_ARGS__)
#define HFT_ERROR(...) HFT_LOG(::hft::LogLevel::Error, __VA_ARGS__)
#define HFT_DEBUG(...) HFT_LOG(::hft::LogLevel::Debug, __VA_ARGS__)
