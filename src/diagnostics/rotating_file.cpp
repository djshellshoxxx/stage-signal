#include "diagnostics/rotating_file.h"

namespace ss {

RotatingFile::RotatingFile(IFileOps& ops, RotationConfig cfg) : ops_(ops), cfg_(std::move(cfg)) {
  if (cfg_.maxFiles < 1) cfg_.maxFiles = 1;
  if (cfg_.maxBytes < 64) cfg_.maxBytes = 64;
}
std::string RotatingFile::pathFor(int i) const {
  return joinPath(cfg_.dir, i == 0 ? cfg_.baseName + cfg_.ext : cfg_.baseName + "." + std::to_string(i) + cfg_.ext);
}
void RotatingFile::rotate() {
  ops_.remove(pathFor(cfg_.maxFiles - 1));
  for (int i = cfg_.maxFiles - 2; i >= 0; --i) if (ops_.fileSize(pathFor(i)) >= 0) ops_.rename(pathFor(i), pathFor(i + 1));
  if (cfg_.maxFiles == 1) ops_.remove(pathFor(0));
  curSize_ = 0; ++rotations_;
}
bool RotatingFile::append(const std::string& rec) {
  if (!dirMade_) { dirMade_ = ops_.createDirs(cfg_.dir); }
  if (curSize_ < 0) { curSize_ = ops_.fileSize(pathFor(0)); if (curSize_ < 0) curSize_ = 0; }
  std::string r = rec;
  if (r.size() > cfg_.maxBytes) { r.resize((size_t)cfg_.maxBytes - 1); r += '\n'; ++truncated_; }
  if (curSize_ > 0 && (uint64_t)curSize_ + r.size() > cfg_.maxBytes) rotate();
  if (!ops_.append(pathFor(0), r.data(), r.size())) { ++failures_; curSize_ = -1; return false; }
  curSize_ += (int64_t)r.size(); return true;
}
std::vector<std::string> RotatingFile::existingPaths() const {
  std::vector<std::string> v;
  for (int i = cfg_.maxFiles - 1; i >= 0; --i) if (ops_.fileSize(pathFor(i)) >= 0) v.push_back(pathFor(i));
  return v;
}
uint64_t RotatingFile::totalBytes() const {
  uint64_t t = 0; for (int i = 0; i < cfg_.maxFiles; ++i) { int64_t s = ops_.fileSize(pathFor(i)); if (s > 0) t += (uint64_t)s; } return t;
}
size_t RotatingFile::removeAll() {
  size_t n = 0; for (int i = 0; i < cfg_.maxFiles; ++i) if (ops_.fileSize(pathFor(i)) >= 0 && ops_.remove(pathFor(i))) ++n;
  curSize_ = 0; return n;
}
size_t RotatingFile::pruneOlderThan(int64_t now, int64_t maxAge) {
  size_t n = 0;
  for (int i = 0; i < cfg_.maxFiles; ++i) {
    int64_t m = ops_.mtimeUnixMs(pathFor(i));
    if (m >= 0 && now - m > maxAge && ops_.remove(pathFor(i))) { ++n; if (i == 0) curSize_ = 0; }
  }
  return n;
}

} // namespace ss
