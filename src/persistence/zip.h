// Minimal ZIP container (STORE writer; STORE+DEFLATE reader) with import-hostile hardening (spec §16/§17).
// Reader validates everything structural at open() before any extraction or large allocation.
#pragma once
#include "persistence/file_ops.h"
#include <functional>
#include <string>
#include <vector>

namespace ss {
namespace persist {

// Member-name policy: rejects absolute / drive / UNC / backslash / '..' / '.' / empty segments / control chars /
// non-ASCII / ':' / reserved Windows device names / trailing dot-space. 'canon' = lower-cased normalized form.
Status checkMemberName(const std::string& raw, std::string& canon, bool* isDir = nullptr);

class ZipWriter {
 public:
  explicit ZipWriter(IWriteFile& out) : out_(out) {}
  Status addBytes(const std::string& name, const void* d, size_t n);
  // Two-pass streaming of a source file (CRC first, then copy). Reports content SHA-256 and size.
  Status addFile(IFileOps& ops, const std::string& name, const std::string& srcPath, std::string* sha256Hex, uint64_t* size);
  Status finish();                        // writes central directory + end record (does not close 'out')
  size_t count() const { return ents_.size(); }
  uint64_t bytesWritten() const { return off_; }
 private:
  struct E { std::string name; uint32_t crc, size, off; };
  Status header(const std::string& name, uint32_t crc, uint32_t size);
  Status put(const void* d, size_t n);
  IWriteFile& out_; std::vector<E> ents_; uint64_t off_ = 0;
};

struct ZipEntry {
  std::string name, canon; bool isDir = false;
  uint16_t method = 0; uint32_t crc = 0;
  uint64_t csize = 0, usize = 0, localOffset = 0, dataOffset = 0;
};

class ZipReader {
 public:
  using NamePolicy = std::function<bool(const std::string& canon, bool isDir)>;
  // Limits: <=10000 entries, <=2 GiB total decompressed, member names per checkMemberName, no encryption/zip64/symlinks,
  // duplicate canonical names rejected, overlapping local records rejected, expansion ratio bounded.
  Status open(IFileOps& ops, const std::string& path, const NamePolicy& allow = nullptr);
  const std::vector<ZipEntry>& entries() const { return ents_; }
  int find(const std::string& canon) const;
  // Streams decompressed bytes to sink; enforces declared size (never more), verifies CRC32.
  Status extract(size_t idx, const std::function<bool(const uint8_t*, size_t)>& sink);
  Status extractToMemory(size_t idx, size_t maxBytes, std::vector<uint8_t>& out);
  uint64_t totalUncompressed() const { return total_; }
 private:
  std::unique_ptr<IReadFile> f_; std::vector<ZipEntry> ents_; uint64_t total_ = 0;
};

} // namespace persist
} // namespace ss
