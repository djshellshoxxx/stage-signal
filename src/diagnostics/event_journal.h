// MOD-16 bounded event journal (spec §17, REQ-X05): in-memory ring + optional JSONL writer (rotating).
// Every event carries BOTH monotonic ns and wall-clock ms. Strings are redacted/truncated on entry. There is no API
// for raw audio, camera frames or key material. Thread-safe; call from workers, not from capture/DSP callbacks.
#pragma once
#include "core/types.h"
#include "core/security.h"
#include "diagnostics/rotating_file.h"
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace ss {

enum class JournalKind : uint8_t {
  Scene, Cue, Mode, TextPublished /*message ID only*/, TempoKey /*<=1 Hz*/, Marker, Device, Adaptation,
  Record, Broadcast, Discontinuity, Note
};
const char* journalKindName(JournalKind k);

struct JournalEvent {
  uint64_t seq = 0;
  Ns monoNs = 0;            // IClock::nowNs
  int64_t wallMs = 0;       // IClock::wallUnixMs
  JournalKind kind = JournalKind::Note;
  std::string subject;      // scene/cue/device/message id etc (<= 96 bytes)
  std::string detail;       // sanitized short text (<= 256 bytes)
  double a = 0, b = 0;      // numeric payload; NaN => unknown (serialized as null)
};

struct JournalConfig {
  size_t ringCapacity = 2048;        // events kept in memory
  size_t pendingCapacity = 1024;     // events waiting for the file writer (drop-oldest beyond this)
  size_t maxSubject = 96, maxDetail = 256;
  Ns tempoKeyMinIntervalNs = kNsPerSec;   // <= 1 Hz
};
struct JournalStats { uint64_t recorded = 0, evictedFromRing = 0, droppedPending = 0, tempoKeyThrottled = 0, writeFailures = 0, written = 0; };

class EventJournal {
 public:
  EventJournal(IClock& clk, JournalConfig cfg = JournalConfig());
  void attachFile(RotatingFile* f);                    // JSONL destination (optional); must outlive journal
  void setRedactor(const sec::Redactor* r);
  uint64_t record(JournalKind k, const std::string& subject, const std::string& detail = std::string(), double a = 0, double b = 0);
  uint64_t marker(const std::string& label) { return record(JournalKind::Marker, label); }
  // Rate-limited to <= 1 Hz; returns false when throttled. Unknown estimates are written as unknown, never as 0.
  bool recordTempoKey(const Estimate& tempo, const Estimate& key);
  size_t flush();                                      // writes pending events to the JSONL file; returns lines written
  std::vector<JournalEvent> snapshot() const;          // ring contents, oldest first
  size_t size() const;
  void clear();                                        // retention control: drop ring + pending (files via RotatingFile::removeAll)
  JournalStats stats() const;
  static std::string toJsonLine(const JournalEvent& e);   // single line, trailing '\n'
 private:
  IClock& clk_; JournalConfig cfg_; RotatingFile* file_ = nullptr; const sec::Redactor* red_ = nullptr;
  mutable std::mutex m_; std::deque<JournalEvent> ring_, pending_; uint64_t seq_ = 0; Ns lastTk_ = 0; bool haveTk_ = false; JournalStats st_;
  uint64_t droppedAnnounced_ = 0;
};

} // namespace ss
