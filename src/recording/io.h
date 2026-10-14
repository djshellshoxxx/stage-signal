// MOD-11 file/disk ports. Writers never touch the OS directly: they use IFileSystem/IFileSink so tests can inject
// full-disk / slow-disk / write-error faults (MemFileSystem) and the real build uses StdFileSystem (stdio).
#pragma once
#include "core/types.h"
#include "recording/bytes.h"
#include <map>
#include <mutex>
#include <memory>
#include <functional>
#include <atomic>

namespace ss {

// One open output file. Sequential write() appends at the end; writeAt() patches earlier bytes (header finalize)
// and leaves the append position at the end. All calls return Err::DiskFull / Err::InternalFault on failure.
struct IFileSink {
  virtual ~IFileSink() = default;
  virtual Status write(const void* data, size_t n) = 0;
  virtual Status writeAt(uint64_t offset, const void* data, size_t n) = 0;
  virtual Status flush() = 0;                  // push to OS (and fflush); durability beyond that is not claimed
  virtual Status close() = 0;
  virtual uint64_t size() const = 0;           // bytes currently in file (append position)
};

struct IFileSystem {
  virtual ~IFileSystem() = default;
  // mode: truncate=create/replace, append=open existing for append (creates if missing), update=open existing r/w.
  enum class Mode { Truncate, Append, Update };
  virtual Status open(const std::string& path, Mode m, std::unique_ptr<IFileSink>& out) = 0;
  virtual Status makeDirs(const std::string& dir) = 0;
  virtual bool exists(const std::string& path) = 0;
  virtual bool fileSize(const std::string& path, uint64_t& out) = 0;
  virtual bool readAll(const std::string& path, rec::Bytes& out) = 0;
  virtual Status resize(const std::string& path, uint64_t size) = 0;   // truncate (recovery)
  virtual Status rename(const std::string& from, const std::string& to) = 0;
  virtual Status remove(const std::string& path) = 0;
  virtual bool writableDir(const std::string& dir, std::string& why) = 0;   // probe: create+delete a temp file
};

// Free space on the volume holding `path` (injectable; Windows implementation lives in src/platform/windows).
struct IDiskInfo {
  virtual ~IDiskInfo() = default;
  virtual bool freeBytes(const std::string& path, uint64_t& out) = 0;
};
struct FakeDiskInfo : IDiskInfo {
  std::atomic<uint64_t> free{0}; std::atomic<bool> fail{false};
  bool freeBytes(const std::string&, uint64_t& out) override { if (fail) return false; out = free; return true; }
};

// Real stdio file system (64-bit offsets). Portable (POSIX + mingw CRT).
IFileSystem& stdFileSystem();
#if !defined(_WIN32)
IDiskInfo& posixDiskInfo();                    // statvfs; Windows build supplies its own IDiskInfo
#endif

// ---- In-memory file system with fault injection (tests / fault seam) ------------------------------------
struct FaultPlan {
  uint64_t capacityBytes = ~0ull;              // total bytes the "volume" accepts; beyond -> DiskFull
  int64_t failWriteAfterBytes = -1;            // after N total written bytes every write fails (Err::InternalFault)
  bool failOpen = false;                       // open() fails (PermissionDenied)
  bool failPatch = false;                      // writeAt fails (header finalize fault)
  std::function<void(size_t)> onWrite;         // slow-disk hook (called per write with size; may sleep / advance a fake clock)
};
class MemFileSystem : public IFileSystem {
 public:
  Status open(const std::string& path, Mode m, std::unique_ptr<IFileSink>& out) override;
  Status makeDirs(const std::string&) override { return Status::Ok(); }
  bool exists(const std::string& p) override;
  bool fileSize(const std::string& p, uint64_t& out) override;
  bool readAll(const std::string& p, rec::Bytes& out) override;
  Status resize(const std::string& p, uint64_t size) override;
  Status rename(const std::string& a, const std::string& b) override;
  Status remove(const std::string& p) override;
  bool writableDir(const std::string&, std::string& why) override { if (plan.failOpen) { why = "not writable (injected)"; return false; } return true; }
  std::vector<std::string> list() const;
  uint64_t totalBytes() const;
  uint64_t writes() const { return writes_; }
  FaultPlan plan;                               // mutate between calls (single-threaded setup) or via lock()
  std::mutex mu;
  struct File { rec::Bytes data; };
  std::map<std::string, std::shared_ptr<File>> files;
  uint64_t written = 0;                         // total payload bytes accepted
  uint64_t writes_ = 0;
  // Simulate a crash: drop the last `n` bytes of a file (unflushed tail loss).
  void chop(const std::string& p, size_t n);
};

} // namespace ss
