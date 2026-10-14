// MOD-16 retention / delete controls for diagnostics data. Secure erasure on SSD is NOT promised (spec §16).
#pragma once
#include "diagnostics/event_journal.h"
#include "diagnostics/logger.h"

namespace ss {

struct RetentionPolicy { int maxAgeDays = 30; /* 0 = keep until deleted/rotated */ };

class DiagnosticsRetention {
 public:
  DiagnosticsRetention(IClock& clk) : clk_(clk) {}
  void setLogger(Logger* l) { logger_ = l; }
  void setJournal(EventJournal* j, RotatingFile* jf) { journal_ = j; journalFile_ = jf; }
  void setPolicy(RetentionPolicy p) { pol_ = p; }
  size_t enforce();       // age-based pruning of log + journal files; returns files removed
  size_t deleteLogs();    // user action: delete all logs
  size_t deleteJournal(); // user action: clear ring + delete journal files
  size_t deleteAll() { return deleteLogs() + deleteJournal(); }
 private:
  IClock& clk_; Logger* logger_ = nullptr; EventJournal* journal_ = nullptr; RotatingFile* journalFile_ = nullptr; RetentionPolicy pol_;
};

} // namespace ss
