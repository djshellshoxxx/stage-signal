// Art-Net 4 (ArtDmx / ArtPoll / ArtPollReply) packet codec, written from the published specification
// (Artistic Licence "Art-Net 4", protocol revision 14). No third-party library. Pure functions, no I/O.
//
// ArtDmx (OpCode 0x5000) byte layout (all multi-byte fields as noted; header = 18 bytes):
//   off  0..7   ID "Art-Net\0"
//   off  8..9   OpCode, LITTLE-endian: 0x00 0x50
//   off 10..11  ProtVer, BIG-endian: 0x00 0x0E (14)
//   off 12      Sequence (0 = disabled, else 1..255 wrapping 255 -> 1)
//   off 13      Physical (input port, informational)
//   off 14      SubUni  = port-address bits 7..0
//   off 15      Net     = port-address bits 14..8 (7 bits)
//   off 16..17  Length, BIG-endian, 2..512, EVEN
//   off 18..    DMX data (Length bytes)
// 15-bit port-address = Net(7) : SubNet(4) : Universe(4). "Universe" shown to users is the WIRE address,
// formatted Net:SubNet:Universe plus the decimal port-address (see formatUniverse) so 0/1-based mistakes are visible.
// Art-Net universe numbers are NOT interchangeable with sACN universe numbers.
// UDP port 6454. Unicast to a known node is the default; broadcast (incl. ArtPoll discovery) is explicit opt-in.
#pragma once
#include "core/types.h"
#include "core/ports.h"
#include <string>
#include <vector>

namespace ss { namespace artnet {

constexpr uint16_t kUdpPort = 6454;
constexpr uint16_t kOpPoll = 0x2000, kOpPollReply = 0x2100, kOpDmx = 0x5000;
constexpr uint8_t kProtVerLo = 14;
constexpr size_t kDmxHeaderSize = 18, kMaxDmxSlots = 512, kPollSize = 14, kPollReplyMinSize = 207;
constexpr uint32_t kMaxPortAddress = 0x7FFF;

inline bool validPortAddress(long v) { return v >= 0 && v <= (long)kMaxPortAddress; }
inline uint8_t nextSequence(uint8_t s) { return s >= 255 ? 1 : (uint8_t)(s + 1); }   // never yields 0
inline uint8_t netOf(uint16_t pa) { return (uint8_t)((pa >> 8) & 0x7F); }
inline uint8_t subUniOf(uint16_t pa) { return (uint8_t)(pa & 0xFF); }
std::string formatUniverse(uint16_t portAddress);   // "0:1:2 (port-address 18)"

struct DmxParams { uint16_t portAddress = 0; uint8_t sequence = 0; uint8_t physical = 0; };
// Returns bytes written, or 0 if invalid (bad port-address, len not even / outside 2..512, cap too small).
size_t buildArtDmx(const DmxParams&, const uint8_t* data, size_t len, uint8_t* out, size_t cap);

struct ArtDmxPacket { DmxParams p; uint16_t length = 0; const uint8_t* data = nullptr; };   // data points into input
bool parseArtDmx(const uint8_t* in, size_t n, ArtDmxPacket& out);

// ArtPoll (14 bytes): ID, OpCode 0x2000 LE, ProtVer BE, Flags, DiagPriority.
size_t buildArtPoll(uint8_t flags, uint8_t diagPriority, uint8_t* out, size_t cap);
bool parseArtPoll(const uint8_t* in, size_t n, uint8_t& flags, uint8_t& diagPriority);

struct PollReply {
  uint8_t ip[4] = {0,0,0,0}; uint16_t port = 0; uint16_t firmware = 0;
  uint8_t netSwitch = 0, subSwitch = 0; uint16_t oem = 0; uint8_t status1 = 0; uint16_t estaMan = 0;
  std::string shortName, longName, nodeReport;
  uint16_t numPorts = 0; uint8_t portTypes[4] = {0,0,0,0}, swIn[4] = {0,0,0,0}, swOut[4] = {0,0,0,0};
  uint8_t style = 0; uint8_t mac[6] = {0,0,0,0,0,0};
  std::string ipString() const;
};
// Parses OpCode 0x2100 (>=207 bytes). Offsets: IP 10, Port(LE) 14, VersInfo 16, NetSwitch 18, SubSwitch 19,
// Oem 20, Status1 23, EstaMan(LE) 24, PortName 26(18), LongName 44(64), NodeReport 108(64), NumPorts 172,
// PortTypes 174, SwIn 186, SwOut 190, Style 200, MAC 201.
bool parsePollReply(const uint8_t* in, size_t n, PollReply& out);

// Heuristic: 255.255.255.255 or any dotted quad whose last octet is 255 (directed/limited broadcast).
bool isBroadcastAddress(const std::string& host);
// Sends an ArtPoll. Broadcast destinations are refused unless allowBroadcastDiscovery is true.
Status sendArtPoll(IDatagramSink& sink, const std::string& host, bool allowBroadcastDiscovery);

}} // namespace ss::artnet
