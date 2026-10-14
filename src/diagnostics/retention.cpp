#include "diagnostics/retention.h"

namespace ss {
size_t DiagnosticsRetention::enforce() {
  if (pol_.maxAgeDays <= 0) return 0;
  int64_t age = (int64_t)pol_.maxAgeDays * 86400000LL; size_t n = 0;
  if (logger_) n += logger_->applyRetention(age);
  if (journal_) journal_->flush();
  if (journalFile_) n += journalFile_->pruneOlderThan(clk_.wallUnixMs(), age);
  return n;
}
size_t DiagnosticsRetention::deleteLogs() { return logger_ ? logger_->deleteAll() : 0; }
size_t DiagnosticsRetention::deleteJournal() {
  if (journal_) journal_->clear();
  return journalFile_ ? journalFile_->removeAll() : 0;
}
}
