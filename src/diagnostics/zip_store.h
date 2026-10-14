// MOD-16 private minimal ZIP writer, STORE (no compression) only, streaming to a sink. 32-bit sizes, <=65535 entries.
// Deliberately independent of the persistence module. Entry names are validated with sec::validateRelPath.
#pragma once
#include "core/types.h"
#include <functional>
#include <string>
#include <vector>

namespace ss {

uint32_t crc32(const void* data, size_t n, uint32_t seed = 0);

class ZipStoreWriter {
 public:
  using Sink = std::function<bool(const char* data, size_t n)>;
  explicit ZipStoreWriter(Sink sink) : sink_(std::move(sink)) {}
  Status addFile(const std::string& name, const std::string& data, int64_t wallUnixMs);
  Status finish();                       // writes central directory + EOCD; writer unusable afterwards
  size_t entries() const { return ents_.size(); }
  uint64_t bytesWritten() const { return off_; }
 private:
  struct Ent { std::string name; uint32_t crc, size, offset; uint16_t dosTime, dosDate; };
  bool put(const std::string& s);
  Sink sink_; std::vector<Ent> ents_; uint64_t off_ = 0; bool done_ = false, failed_ = false;
};

} // namespace ss
