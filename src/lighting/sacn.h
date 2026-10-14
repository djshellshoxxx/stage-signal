// ANSI E1.31-2018 (sACN) DATA packet builder/parser written from the published standard. No ETC library.
// Pure functions, no I/O. UDP port 5568. Universe 1..63999. Unicast OR multicast 239.255.<hi>.<lo>.
// (Synchronization packets are NOT implemented; sync address is always 0. Do not rely on multi-universe sync.)
//
// Packet layout for N slots (total 126+N bytes, 638 for N=512). All multi-byte fields BIG-endian.
// ---- Root layer (ACN, E1.17) ----
//   0..1    Preamble size           0x0010
//   2..3    Post-amble size         0x0000
//   4..15   ACN packet identifier   "ASC-E1.17\0\0\0"
//   16..17  Flags&Length            0x7000 | (total-16)        (root PDU length, from offset 16)
//   18..21  Root vector             0x00000004 (VECTOR_ROOT_E131_DATA)
//   22..37  CID                     16 bytes, sender component id (stable per source)
// ---- Framing layer ----
//   38..39  Flags&Length            0x7000 | (total-38)
//   40..43  Framing vector          0x00000002 (VECTOR_E131_DATA_PACKET)
//   44..107 Source name             64 bytes UTF-8, NUL terminated/padded
//   108     Priority                0..200 (default 100)
//   109..110 Synchronization addr   0 (unused)
//   111     Sequence number         per-universe, wraps 0..255
//   112     Options                 bit7 preview, bit6 stream-terminated, bit5 force-sync
//   113..114 Universe               1..63999
// ---- DMP layer ----
//   115..116 Flags&Length           0x7000 | (total-115)
//   117     DMP vector              0x02 (SET_PROPERTY)
//   118     Address & data type     0xA1
//   119..120 First property addr    0x0000
//   121..122 Address increment      0x0001
//   123..124 Property value count   N+1
//   125     DMX start code          0x00
//   126..   Slot data (N, 1..512)
#pragma once
#include "core/types.h"
#include <array>
#include <string>

namespace ss { namespace sacn {

constexpr uint16_t kUdpPort = 5568;
constexpr uint32_t kMinUniverse = 1, kMaxUniverse = 63999;
constexpr size_t kMaxSlots = 512, kHeaderSize = 126, kMaxPacket = kHeaderSize + kMaxSlots, kSourceNameBytes = 64;
constexpr uint8_t kDefaultPriority = 100, kMaxPriority = 200;
constexpr int kTerminationRepeats = 3;                       // E1.31: send stream-terminated packets 3 times
constexpr uint8_t kOptPreview = 0x80, kOptTerminated = 0x40, kOptForceSync = 0x20;

using Cid = std::array<uint8_t, 16>;
inline bool validUniverse(long u) { return u >= (long)kMinUniverse && u <= (long)kMaxUniverse; }
Cid makeCid(uint64_t seed);                                   // deterministic RFC4122-v4-shaped CID from a seed
std::string multicastAddress(uint32_t universe);              // "239.255.hi.lo"; "" if universe invalid

struct SourceInfo { Cid cid{}; std::string name = "StageSignal"; uint8_t priority = kDefaultPriority; };
struct DataParams { uint16_t universe = 1; uint8_t sequence = 0; uint8_t options = 0; uint16_t syncAddress = 0; };

// Returns bytes written or 0 on invalid input (universe range, n outside 1..512, priority >200, cap too small).
size_t buildDataPacket(const SourceInfo&, const DataParams&, const uint8_t* slots, size_t n, uint8_t* out, size_t cap);

struct ParsedData { Cid cid{}; std::string name; uint8_t priority = 0; DataParams p; uint8_t startCode = 0; uint16_t slotCount = 0; const uint8_t* slots = nullptr; };
bool parseDataPacket(const uint8_t* in, size_t n, ParsedData& out);

}} // namespace ss::sacn
