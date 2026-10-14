// MOD-16 injectable file operations (std::filesystem backed + in-memory fake for tests). Paths use '/' separators.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <memory>

namespace ss {

struct IFileOps {
  virtual ~IFileOps() = default;
  virtual bool createDirs(const std::string& dir) = 0;
  virtual bool append(const std::string& path, const char* d, size_t n) = 0;       // creates file; false on any failure
  virtual bool writeAll(const std::string& path, const std::string& data) = 0;     // replace
  virtual bool readAll(const std::string& path, std::string& out, size_t maxBytes) = 0;  // false if missing or larger than maxBytes
  virtual int64_t fileSize(const std::string& path) = 0;                           // -1 if missing
  virtual int64_t mtimeUnixMs(const std::string& path) = 0;                        // -1 if missing/unknown
  virtual bool rename(const std::string& from, const std::string& to) = 0;         // replaces target
  virtual bool remove(const std::string& path) = 0;                                // true if gone afterwards
  virtual std::vector<std::string> listDir(const std::string& dir) = 0;            // regular file names only, sorted
};

std::string joinPath(const std::string& a, const std::string& b);

// Real implementation using std::filesystem / fstream (binary). Stateless.
IFileOps& stdFileOps();

// In-memory fake: deterministic, supports quota (disk full) and failure injection.
class MemFileOps : public IFileOps {
 public:
  struct Entry { std::string data; int64_t mtime = 0; };
  std::map<std::string, Entry> files;
  int64_t nowMs = 0;                 // stamped on write
  uint64_t quotaBytes = UINT64_MAX;  // total bytes across files
  bool failAll = false;
  uint64_t totalBytes() const { uint64_t t = 0; for (auto& f : files) t += f.second.data.size(); return t; }
  bool createDirs(const std::string&) override { return !failAll; }
  bool append(const std::string& p, const char* d, size_t n) override;
  bool writeAll(const std::string& p, const std::string& data) override;
  bool readAll(const std::string& p, std::string& out, size_t maxBytes) override;
  int64_t fileSize(const std::string& p) override { auto i = files.find(p); return i == files.end() ? -1 : (int64_t)i->second.data.size(); }
  int64_t mtimeUnixMs(const std::string& p) override { auto i = files.find(p); return i == files.end() ? -1 : i->second.mtime; }
  bool rename(const std::string& a, const std::string& b) override;
  bool remove(const std::string& p) override { if (failAll) return false; files.erase(p); return true; }
  std::vector<std::string> listDir(const std::string& dir) override;
};

} // namespace ss
