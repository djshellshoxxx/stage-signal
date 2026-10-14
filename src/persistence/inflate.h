// Minimal streaming DEFLATE (RFC 1951) decoder, hardened for hostile input (bounded output, bounded window).
#pragma once
#include "core/types.h"
#include <functional>

namespace ss {
namespace persist {

struct InflateIO {
  std::function<size_t(uint8_t* buf, size_t n)> in;           // returns bytes provided (0 = end of input)
  std::function<bool(const uint8_t* d, size_t n)> out;        // return false to abort (e.g. write failure)
};
// Decodes until the final block. Fails with ResourceExhausted if output would exceed maxOut,
// DecodeFailure on corrupt/truncated streams. 'produced' receives the number of bytes emitted.
Status inflateStream(InflateIO& io, uint64_t maxOut, uint64_t& produced);

} // namespace persist
} // namespace ss
