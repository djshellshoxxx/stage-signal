#include "diagnostics/event_journal.h"
#include "diagnostics/time_util.h"
#include "nlohmann/json.hpp"
#include <cmath>

namespace ss {

const char* journalKindName(JournalKind k) {
  static const char* n[] = {"scene", "cue", "mode", "text", "tempo_key", "marker", "device", "adaptation", "record", "broadcast", "discontinuity", "note"};
  return (size_t)k < sizeof(n) / sizeof(*n) ? n[(size_t)k] : "?";
}

EventJournal::EventJournal(IClock& c, JournalConfig cfg) : clk_(c), cfg_(cfg) {
  if (cfg_.ringCapacity < 1) cfg_.ringCapacity = 1;
  if (cfg_.pendingCapacity < 1) cfg_.pendingCapacity = 1;
}
void EventJournal::attachFile(RotatingFile* f) { std::lock_guard<std::mutex> g(m_); file_ = f; }
void EventJournal::setRedactor(const sec::Redactor* r) { std::lock_guard<std::mutex> g(m_); red_ = r; }

static std::string clip(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  size_t c = n > 3 ? n - 3 : 0; while (c > 0 && ((unsigned char)s[c] & 0xC0) == 0x80) --c;
  return s.substr(0, c) + "...";
}

uint64_t EventJournal::record(JournalKind k, const std::string& subject, const std::string& detail, double a, double b) {
  JournalEvent e; e.kind = k; e.monoNs = clk_.nowNs(); e.wallMs = clk_.wallUnixMs(); e.a = a; e.b = b;
  std::lock_guard<std::mutex> g(m_);
  auto san = [&](const std::string& s, size_t lim) { return sec::sanitizeLibraryError(red_ ? red_->apply(clip(s, lim * 4)) : clip(s, lim * 4), lim); };
  e.subject = san(subject, cfg_.maxSubject); e.detail = san(detail, cfg_.maxDetail);
  e.seq = ++seq_; ++st_.recorded;
  if (ring_.size() >= cfg_.ringCapacity) { ring_.pop_front(); ++st_.evictedFromRing; }
  ring_.push_back(e);
  if (file_) {
    if (pending_.size() >= cfg_.pendingCapacity) { pending_.pop_front(); ++st_.droppedPending; }
    pending_.push_back(e);
  }
  return e.seq;
}

static std::string keyName(const Estimate& k) {
  static const char* pc[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  int p = ((int)std::lround(k.value) % 12 + 12) % 12; return std::string(pc[p]) + (k.minor ? " minor" : " major");
}
bool EventJournal::recordTempoKey(const Estimate& tempo, const Estimate& key) {
  Ns now = clk_.nowNs();
  { std::lock_guard<std::mutex> g(m_);
    if (haveTk_ && now - lastTk_ < cfg_.tempoKeyMinIntervalNs) { ++st_.tempoKeyThrottled; return false; }
    haveTk_ = true; lastTk_ = now; }
  char buf[160]; std::string d;
  if (tempo.valid) std::snprintf(buf, sizeof buf, "bpm=%.1f", tempo.value); else std::snprintf(buf, sizeof buf, "bpm=unknown");
  d = buf;
  std::snprintf(buf, sizeof buf, " bpm_conf=%.2f bpm_age_ms=%lld bpm_src=%d", tempo.confidence, (long long)(tempo.ageNs / kNsPerMs), (int)tempo.source);
  d += buf;
  std::snprintf(buf, sizeof buf, " tonality=%s tonality_conf=%.2f tonality_age_ms=%lld tonality_src=%d", key.valid ? keyName(key).c_str() : "unknown", key.confidence, (long long)(key.ageNs / kNsPerMs), (int)key.source);
  d += buf;
  record(JournalKind::TempoKey, "snapshot", d, tempo.valid ? tempo.value : std::nan(""), key.valid ? key.value + (key.minor ? 12 : 0) : std::nan(""));
  return true;
}

std::string EventJournal::toJsonLine(const JournalEvent& e) {
  nlohmann::json j;
  j["seq"] = e.seq; j["mono_ns"] = e.monoNs; j["wall_ms"] = e.wallMs; j["wall_iso"] = isoUtc(e.wallMs);
  j["kind"] = journalKindName(e.kind); j["subject"] = e.subject; j["detail"] = e.detail;
  j["a"] = std::isfinite(e.a) ? nlohmann::json(e.a) : nlohmann::json(nullptr);
  j["b"] = std::isfinite(e.b) ? nlohmann::json(e.b) : nlohmann::json(nullptr);
  return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

size_t EventJournal::flush() {
  std::deque<JournalEvent> todo; RotatingFile* f;
  { std::lock_guard<std::mutex> g(m_); f = file_; todo.swap(pending_);
    if (f && st_.droppedPending > droppedAnnounced_) {          // make loss visible in the stream itself
      JournalEvent n; n.kind = JournalKind::Note; n.monoNs = clk_.nowNs(); n.wallMs = clk_.wallUnixMs(); n.seq = ++seq_;
      n.subject = "journal"; n.detail = "pending queue overflow, events dropped=" + std::to_string(st_.droppedPending - droppedAnnounced_);
      droppedAnnounced_ = st_.droppedPending; todo.push_front(n);
    } }
  if (!f) return 0;
  size_t n = 0, fail = 0;
  for (auto& e : todo) { if (f->append(toJsonLine(e))) ++n; else ++fail; }
  std::lock_guard<std::mutex> g(m_); st_.written += n; st_.writeFailures += fail;
  return n;
}
std::vector<JournalEvent> EventJournal::snapshot() const { std::lock_guard<std::mutex> g(m_); return std::vector<JournalEvent>(ring_.begin(), ring_.end()); }
size_t EventJournal::size() const { std::lock_guard<std::mutex> g(m_); return ring_.size(); }
void EventJournal::clear() { std::lock_guard<std::mutex> g(m_); ring_.clear(); pending_.clear(); }
JournalStats EventJournal::stats() const { std::lock_guard<std::mutex> g(m_); return st_; }

} // namespace ss
