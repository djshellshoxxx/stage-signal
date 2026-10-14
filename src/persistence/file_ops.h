// File-operation port for persistence (MOD-13): default std::filesystem/stdio implementation plus a
// fault-injecting wrapper for crash-at-write tests (REQ-X01). Paths are UTF-8. No Windows headers.
#pragma once
#include "core/types.h"
#include <memory>
#include <string>
#include <vector>

namespace ss {
namespace persist {

struct IWriteFile {
  virtual ~IWriteFile() = default;
  virtual Status write(const void* d, size_t n) = 0;
  virtual Status flushAndClose() = 0;      // flush to stable storage (fsync/_commit) then close
};
struct IReadFile {
  virtual ~IReadFile() = default;
  virtual uint64_t size() const = 0;
  virtual Status seek(uint64_t off) = 0;
  virtual Status read(void* d, size_t n, size_t& got) = 0;   // got < n only at EOF
};
struct IFileOps {
  virtual ~IFileOps() = default;
  virtual Status openWrite(const std::string& path, std::unique_ptr<IWriteFile>& out) = 0;   // create/truncate
  virtual Status openRead(const std::string& path, std::unique_ptr<IReadFile>& out) = 0;
  virtual Status rename(const std::string& from, const std::string& to) = 0;   // atomic replace for files; dir rename needs absent target
  virtual Status remove(const std::string& path) = 0;                           // ok if missing
  virtual Status removeAll(const std::string& path) = 0;                        // recursive, ok if missing
  virtual Status createDirs(const std::string& path) = 0;
  virtual Status createPrivateDir(const std::string& path) = 0;                 // fails if it exists; owner-only where supported
  virtual bool exists(const std::string& path) = 0;
  virtual bool isSymlink(const std::string& path) = 0;
  virtual Status listDir(const std::string& path, std::vector<std::string>& names) = 0;   // names only
};
IFileOps& defaultFileOps();

// Helpers built on IFileOps.
Status writeFileAll(IFileOps& ops, const std::string& path, const void* d, size_t n);       // write + flush + close
Status readFileAll(IFileOps& ops, const std::string& path, size_t maxBytes, std::vector<uint8_t>& out);   // ResourceExhausted if larger
Status copyFile(IFileOps& ops, const std::string& from, const std::string& to);
std::string parentDir(const std::string& path);
std::string joinPath(const std::string& a, const std::string& b);
std::string fileName(const std::string& path);

// Fault injection: wraps another IFileOps. A "crash" leaves the partial file on disk and returns an error.
class FaultFileOps : public IFileOps {
 public:
  explicit FaultFileOps(IFileOps& base) : base_(base) {}
  // Fail the Nth rename (1-based, 0 = never).
  int failRenameNth = 0;
  // After this many bytes written in total, the write that crosses it is truncated at the limit and fails (-1 = never).
  int64_t crashAfterBytes = -1;
  // Fail flushAndClose (after data written).
  bool failFlush = false;
  // Only apply byte-crash to paths containing this substring (empty = all).
  std::string onlyPathContaining;
  int renames = 0; int64_t bytesWritten = 0; int opens = 0; std::vector<std::string> writtenPaths;
  Status openWrite(const std::string& path, std::unique_ptr<IWriteFile>& out) override;
  Status openRead(const std::string& path, std::unique_ptr<IReadFile>& out) override { return base_.openRead(path, out); }
  Status rename(const std::string& from, const std::string& to) override;
  Status remove(const std::string& p) override { return base_.remove(p); }
  Status removeAll(const std::string& p) override { return base_.removeAll(p); }
  Status createDirs(const std::string& p) override { return base_.createDirs(p); }
  Status createPrivateDir(const std::string& p) override { return base_.createPrivateDir(p); }
  bool exists(const std::string& p) override { return base_.exists(p); }
  bool isSymlink(const std::string& p) override { return base_.isSymlink(p); }
  Status listDir(const std::string& p, std::vector<std::string>& n) override { return base_.listDir(p, n); }
 private:
  IFileOps& base_;
};

} // namespace persist
} // namespace ss
