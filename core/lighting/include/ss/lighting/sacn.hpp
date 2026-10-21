// ANSI E1.31-2018 (sACN) data packet builder/parser, written in-tree. Synchronization packets are NOT
// implemented and the sync address is always 0 (spec section 14: do not depend on multi-universe sync).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/lighting/addressing.hpp"

namespace ss::lighting::sacn {

constexpr std::size_t kDataOffset = 126;           // first DMX slot (slot 1); start code sits at 125
constexpr std::size_t kMaxPacketSize = 638;        // 126 + 512
constexpr std::uint8_t kOptPreview = 0x80, kOptStreamTerminated = 0x40, kOptForceSync = 0x20;
using Cid = std::array<std::uint8_t, 16>;

struct DataParams {
    Cid cid{};                                     // must not be all zero
    std::string sourceName;                        // UTF-8, truncated to 63 bytes on a code point boundary
    std::uint8_t priority = kSacnDefaultPriority;  // 0..200
    std::uint8_t sequence = 0;                     // wraps naturally 0..255
    bool preview = false;                          // Preview_Data: NOT a safety mechanism; dry-run uses mock sinks
    bool streamTerminated = false;
    SacnUniverse universe;
};

// 1..512 data slots, start code 0. Bounds are checked before allocation.
Result<std::vector<std::uint8_t>> buildData(const DataParams& p, const std::uint8_t* data, std::size_t len);

// Source name -> at most 63 bytes (+ NUL) of well-formed UTF-8 (never splits a code point).
std::string truncateSourceName(const std::string& s);

struct ParsedData {
    DataParams params;
    std::uint8_t options = 0;
    std::uint16_t syncAddress = 0;
    std::vector<std::uint8_t> data;                // slots 1..N (start code excluded)
};
Result<ParsedData> parseData(const std::uint8_t* pkt, std::size_t len);

}  // namespace ss::lighting::sacn
