#include "hft/log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>

namespace hft {

const char* to_string(LogLevel l) {
  switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Off: return "OFF";
  }
  return "?";
}

bool parse_log_level(const std::string& text, LogLevel& out) {
  std::string t;
  t.reserve(text.size());
  for (char c : text) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

  if (t == "trace") { out = LogLevel::Trace; return true; }
  if (t == "debug") { out = LogLevel::Debug; return true; }
  if (t == "info") { out = LogLevel::Info; return true; }
  if (t == "warn" || t == "warning") { out = LogLevel::Warn; return true; }
  if (t == "error") { out = LogLevel::Error; return true; }
  if (t == "off" || t == "none") { out = LogLevel::Off; return true; }
  return false;
}

Logger::Logger(LogLevel min_level, std::FILE* out, std::size_t capacity)
    : min_level_(min_level), out_(out ? out : stderr), queue_(capacity) {}

Logger::~Logger() { stop(); }

void Logger::start() {
  if (running_.exchange(true, std::memory_order_acq_rel)) return;  // already started
  writer_ = std::thread([this] { run(); });
}

void Logger::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  if (writer_.joinable()) writer_.join();

  // Drain anything the writer thread did not get to before it observed the
  // stop flag. Losing the last few lines of a shutdown log is exactly when it
  // would hurt most.
  LogRecord rec;
  while (queue_.try_pop(rec)) write_one(rec);
  std::fflush(out_);
}

void Logger::emit(const LogRecord& rec) {
  if (running_.load(std::memory_order_relaxed)) {
    // Drop rather than block: a stalled log must never stall trading.
    queue_.push_or_drop(rec);
    return;
  }
  // No writer thread running (e.g. during startup or after stop) -- write
  // synchronously so the message is not silently lost. This path is never on
  // the tick loop.
  write_one(rec);
}

void Logger::log(LogLevel l, const char* message) {
  LogRecord rec;
  rec.ts_ns = now_ns();
  rec.level = l;
  rec.message = message;
  rec.field_count = 0;
  emit(rec);
}

void Logger::log(LogLevel l, const char* message, const char* k0, double v0) {
  LogRecord rec;
  rec.ts_ns = now_ns();
  rec.level = l;
  rec.message = message;
  rec.key[0] = k0;
  rec.value[0] = v0;
  rec.field_count = 1;
  emit(rec);
}

void Logger::log(LogLevel l, const char* message, const char* k0, double v0, const char* k1,
                 double v1) {
  LogRecord rec;
  rec.ts_ns = now_ns();
  rec.level = l;
  rec.message = message;
  rec.key[0] = k0;
  rec.value[0] = v0;
  rec.key[1] = k1;
  rec.value[1] = v1;
  rec.field_count = 2;
  emit(rec);
}

void Logger::log(LogLevel l, const char* message, const char* k0, double v0, const char* k1,
                 double v1, const char* k2, double v2) {
  LogRecord rec;
  rec.ts_ns = now_ns();
  rec.level = l;
  rec.message = message;
  rec.key[0] = k0;
  rec.value[0] = v0;
  rec.key[1] = k1;
  rec.value[1] = v1;
  rec.key[2] = k2;
  rec.value[2] = v2;
  rec.field_count = 3;
  emit(rec);
}

void Logger::write_one(const LogRecord& rec) {
  if (rec.message == nullptr) return;

  // Timestamps are monotonic nanoseconds, printed as seconds since process
  // start. Wall-clock time is deliberately not read on the hot path.
  const double secs = static_cast<double>(rec.ts_ns) / 1e9;
  std::fprintf(out_, "[%14.6f] %-5s %s", secs, to_string(rec.level), rec.message);
  for (std::uint8_t i = 0; i < rec.field_count && i < 3; ++i) {
    if (rec.key[i] == nullptr) continue;
    const double v = rec.value[i];
    // Integers are far more common than fractions here; print them cleanly.
    if (v == static_cast<double>(static_cast<long long>(v))) {
      std::fprintf(out_, " %s=%lld", rec.key[i], static_cast<long long>(v));
    } else {
      std::fprintf(out_, " %s=%.4f", rec.key[i], v);
    }
  }
  std::fputc('\n', out_);
  written_.fetch_add(1, std::memory_order_relaxed);
}

void Logger::run() {
  LogRecord rec;
  while (running_.load(std::memory_order_acquire)) {
    bool did_work = false;
    // Batch: draining several records per wake amortises the flush.
    for (int i = 0; i < 256 && queue_.try_pop(rec); ++i) {
      write_one(rec);
      did_work = true;
    }
    if (did_work) {
      std::fflush(out_);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

Logger& default_logger() {
  static Logger instance;
  return instance;
}

}  // namespace hft
