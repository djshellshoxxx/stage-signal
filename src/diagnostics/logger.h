// MOD-16 sanitized rotating text logger (spec §16, REQ-B03). Every message is redacted + single-lined before it
// reaches memory buffers or disk. Thread-safe; NOT for audio/render hot paths (takes a mutex, may touch disk).
#pragma once
#include "core/types.h"
#include "core/security.h"
#include "diagnostics/rotating_file.h"
#include <mutex>
#include <string>

namespace ss {

enum class LogLevel : uint8_t { Debug, Info, Warn, Error };
const char* logLevelName(LogLevel l);

struct LoggerConfig {
  LogLevel minLevel = LogLevel::Info;
  size_t maxLineBytes = 1024;      // after sanitizing; longer messages are truncated with "..."
  size_t bufferBytes = 8192;       // flushed when exceeded, on Warn/Error, or flush()
};
struct LoggerStats { uint64_t lines = 0, filteredByLevel = 0, flushFailures = 0, bytesDroppedOnFailure = 0; };

class Logger {
 public:
  Logger(IClock& clk, RotatingFile& file, LoggerConfig cfg = LoggerConfig());
  ~Logger();
  void setRedactor(const sec::Redactor* r);             // extra exact secrets (stream keys, user names); pointer must outlive logger
  void setMinLevel(LogLevel l);
  void log(LogLevel lvl, const std::string& component, const std::string& msg);
  void debug(const std::string& c, const std::string& m) { log(LogLevel::Debug, c, m); }
  void info(const std::string& c, const std::string& m) { log(LogLevel::Info, c, m); }
  void warn(const std::string& c, const std::string& m) { log(LogLevel::Warn, c, m); }
  void error(const std::string& c, const std::string& m) { log(LogLevel::Error, c, m); }
  bool flush();
  // Retention / delete controls.
  size_t deleteAll();                                    // drops buffer + all rotated files
  size_t applyRetention(int64_t maxAgeMs);               // remove log files older than maxAgeMs (by mtime, via file ops)
  LoggerStats stats() const;
  RotatingFile& file() { return file_; }
 private:
  bool flushLocked();
  IClock& clk_; RotatingFile& file_; LoggerConfig cfg_; const sec::Redactor* red_ = nullptr;
  mutable std::mutex m_; std::string buf_; LoggerStats st_;
};

} // namespace ss
