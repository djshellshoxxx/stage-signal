// Art-Net 4 packet builders/parsers, written in-tree (no third-party code). See docs/specs/lighting.md for
// exactly which facts were verified against which source and which are from the published spec text only.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/lighting/addressing.hpp"

namespace ss::lighting::artnet {

constexpr std::uint16_t kOpPoll = 0x2000, kOpPollReply = 0x2100, kOpDmx = 0x5000;   // little-endian on the wire
constexpr std::size_t kHeaderSize = 18;            // "Art-Net\0"(8) op(2) ver(2) seq phys subuni net len(2)
constexpr std::size_t kPollSize = 14;
constexpr std::size_t kPollReplyMinSize = 207;     // through Mac[6]; Art-Net 4 replies are 239 bytes
constexpr std::size_t kPollReplyFullSize = 239;
extern const std::array<std::uint8_t, 8> kId;      // "Art-Net\0"

struct DmxParams {
    ArtNetPortAddress address;
    std::uint8_t sequence = 0;       // 0 = sequencing disabled; senders count 1..255 and skip 0 on wrap
    std::uint8_t physical = 0;       // informational physical input port
};

// Length must be even, 2..512 (Art-Net data length rule). Bounds are checked before allocation.
Result<std::vector<std::uint8_t>> buildDmx(const DmxParams& p, const std::uint8_t* data, std::size_t len);

// Next Art-Net sequence number: 1..255, wraps to 1 (0 means "disabled" and is never produced).
constexpr std::uint8_t nextSequence(std::uint8_t cur) { return cur >= 255 ? std::uint8_t{1} : static_cast<std::uint8_t>(cur + 1); }

struct ParsedDmx {
    DmxParams params;
    std::vector<std::uint8_t> data;
};
Result<ParsedDmx> parseDmx(const std::uint8_t* pkt, std::size_t len);

// ---- Discovery (opt-in: the engine never emits ArtPoll by itself) ----------------------------
struct PollParams {
    std::uint8_t talkToMe = 0;       // flags; 0 = reply only to this poll, broadcast reply, no diagnostics
    std::uint8_t diagPriority = 0x10;
};
std::vector<std::uint8_t> buildPoll(const PollParams& p);

struct PollReplyInfo {
    Ipv4 ip;
    std::uint16_t port = 0;
    std::uint8_t versionHi = 0, versionLo = 0;
    std::uint8_t netSwitch = 0, subSwitch = 0;
    std::uint16_t oem = 0, estaMan = 0;
    std::uint8_t status1 = 0;
    std::string shortName, longName, nodeReport;       // sanitised: printable ASCII only
    std::uint16_t numPorts = 0;
    std::array<std::uint8_t, 4> portTypes{{0, 0, 0, 0}}, swIn{{0, 0, 0, 0}}, swOut{{0, 0, 0, 0}};
    std::uint8_t style = 0;
    std::array<std::uint8_t, 6> mac{{0, 0, 0, 0, 0, 0}};
    Ipv4 bindIp;
    std::uint8_t bindIndex = 0;
    bool hasBind = false;                              // packet long enough to carry BindIp/BindIndex
    // Port-Address of output / input port i (0..3), combining NetSwitch, SubSwitch and the 4 bit SwOut/SwIn.
    ArtNetPortAddress outputAddress(unsigned i) const;
    ArtNetPortAddress inputAddress(unsigned i) const;
    bool portCanOutput(unsigned i) const { return i < 4 && (portTypes[i] & 0x80) != 0; }   // PortTypes bit 7
    bool portCanInput(unsigned i) const { return i < 4 && (portTypes[i] & 0x40) != 0; }    // PortTypes bit 6
};
Result<PollReplyInfo> parsePollReply(const std::uint8_t* pkt, std::size_t len);

}  // namespace ss::lighting::artnet
