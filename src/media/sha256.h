// Own SHA-256 (FIPS 180-4), portable, no dependencies. Used by the asset manifest verifier (REQ-S15).
#pragma once
#include "core/types.h"
#include <cstdint>
#include <string>

namespace ss { namespace media {

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const void* data, size_t n);
  void final(uint8_t out[32]);               // finalizes; call reset() to reuse
  std::string finalHex();                    // lowercase hex
 private:
  void block(const uint8_t* p);
  uint32_t h_[8]; uint8_t buf_[64]; uint64_t len_ = 0; size_t fill_ = 0;
};
std::string sha256Hex(const void* data, size_t n);
Status sha256FileHex(const std::string& path, std::string& hexOut, uint64_t* sizeOut = nullptr);   // streaming, 64 KiB chunks; path is trusted (not validateLocalPath'd)
bool isHex64(const std::string& s);                        // 64 lowercase hex chars

}} // namespace ss::media
