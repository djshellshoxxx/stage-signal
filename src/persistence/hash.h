// SHA-256 and CRC-32 (portable, no deps). Used for save checksums, media content hashes and ZIP member checks.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ss {
namespace persist {

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const void* d, size_t n);
  std::string finalHex();            // lower-case hex; object must be reset() before reuse
  static std::string hex(const void* d, size_t n);
 private:
  void block(const uint8_t* p);
  uint32_t h_[8]; uint8_t buf_[64]; size_t bufLen_ = 0; uint64_t total_ = 0;
};

uint32_t crc32Update(uint32_t crc, const void* d, size_t n);   // start with 0
inline uint32_t crc32(const void* d, size_t n) { return crc32Update(0, d, n); }

} // namespace persist
} // namespace ss
