// MOD-16 size-rotated append-only text file set: base.log, base.1.log ... (spec §16: 5 x 5 MiB).
#pragma once
#include "diagnostics/file_ops.h"
#include <string>
#include <vector>

namespace ss {

struct RotationConfig {
  std::string dir;                       // directory (created on first use)
  std::string baseName = "stagesignal";  // file stem
  std::string ext = ".log";
  uint64_t maxBytes = 5ull << 20;        // per file
  int maxFiles = 5;                      // total files kept including current => <= 25 MiB
};

class RotatingFile {
 public:
  RotatingFile(IFileOps& ops, RotationConfig cfg);
  // Appends one record (should end in '\n'). Rotates first if it would overflow the current file.
  // Never throws; returns false (and counts a failure) when the filesystem refuses (disk full etc.).
  bool append(const std::string& record);
  std::string pathFor(int index) const;           // 0 = current, 1 = newest rotated ...
  std::vector<std::string> existingPaths() const; // oldest first (chronological), only files that exist
  uint64_t totalBytes() const;
  size_t removeAll();                             // retention/delete control; returns files removed
  size_t pruneOlderThan(int64_t wallNowMs, int64_t maxAgeMs);   // by mtime; never removes the current file's newest data if younger
  uint64_t rotations() const { return rotations_; }
  uint64_t failures() const { return failures_; }
  uint64_t truncatedRecords() const { return truncated_; }
  const RotationConfig& config() const { return cfg_; }
 private:
  void rotate();
  IFileOps& ops_; RotationConfig cfg_; int64_t curSize_ = -1; bool dirMade_ = false;
  uint64_t rotations_ = 0, failures_ = 0, truncated_ = 0;
};

} // namespace ss
