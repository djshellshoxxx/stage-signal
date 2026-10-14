#include "diagnostics/logger.h"
#include "diagnostics/time_util.h"

namespace ss {

const char* logLevelName(LogLevel l) { switch (l) { case LogLevel::Debug: return "DEBUG"; case LogLevel::Info: return "INFO"; case LogLevel::Warn: return "WARN"; default: return "ERROR"; } }

Logger::Logger(IClock& clk, RotatingFile& f, LoggerConfig cfg) : clk_(clk), file_(f), cfg_(cfg) {}
Logger::~Logger() { flush(); }
void Logger::setRedactor(const sec::Redactor* r) { std::lock_guard<std::mutex> l(m_); red_ = r; }
void Logger::setMinLevel(LogLevel l) { std::lock_guard<std::mutex> g(m_); cfg_.minLevel = l; }

void Logger::log(LogLevel lvl, const std::string& component, const std::string& msg) {
  std::lock_guard<std::mutex> g(m_);
  if (lvl < cfg_.minLevel) { ++st_.filteredByLevel; return; }
  std::string comp;
  for (char c : component) { if (comp.size() >= 24) break; comp += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-') ? c : '_'; }
  if (comp.empty()) comp = "-";
  std::string clean = sec::sanitizeLibraryError(red_ ? red_->apply(msg) : msg, cfg_.maxLineBytes);
  char head[96]; std::snprintf(head, sizeof head, " +%lldms ", (long long)(clk_.nowNs() / kNsPerMs));
  std::string line = isoUtc(clk_.wallUnixMs()) + head + logLevelName(lvl) + " " + comp + " " + clean + "\n";
  ++st_.lines;
  if (buf_.size() + line.size() > cfg_.bufferBytes) flushLocked();
  buf_ += line;
  if (lvl >= LogLevel::Warn || buf_.size() >= cfg_.bufferBytes) flushLocked();
}
bool Logger::flushLocked() {
  if (buf_.empty()) return true;
  // One rotation-aware append per line so a rotation never splits a line.
  bool ok = true; size_t pos = 0;
  while (pos < buf_.size()) {
    size_t e = buf_.find('\n', pos); e = (e == std::string::npos) ? buf_.size() : e + 1;
    if (!file_.append(buf_.substr(pos, e - pos))) { ok = false; st_.bytesDroppedOnFailure += e - pos; }
    pos = e;
  }
  if (!ok) ++st_.flushFailures;
  buf_.clear(); return ok;   // bounded: on failure data is dropped and counted, never accumulated
}
bool Logger::flush() { std::lock_guard<std::mutex> g(m_); return flushLocked(); }
size_t Logger::deleteAll() { std::lock_guard<std::mutex> g(m_); buf_.clear(); return file_.removeAll(); }
size_t Logger::applyRetention(int64_t maxAgeMs) { std::lock_guard<std::mutex> g(m_); flushLocked(); return file_.pruneOlderThan(clk_.wallUnixMs(), maxAgeMs); }
LoggerStats Logger::stats() const { std::lock_guard<std::mutex> g(m_); return st_; }

} // namespace ss
