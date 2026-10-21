#include "ss/lighting/artnet.hpp"

#include <cstring>

namespace ss::lighting::artnet {

const std::array<std::uint8_t, 8> kId{{'A', 'r', 't', '-', 'N', 'e', 't', 0}};

namespace {
Error bad(const char* why) { return makeError(ErrorCategory::InvalidArgument, why); }

void putHeader(std::vector<std::uint8_t>& v, std::uint16_t op) {
    v.insert(v.end(), kId.begin(), kId.end());
    v.push_back(static_cast<std::uint8_t>(op & 0xFF));          // OpCode low byte first
    v.push_back(static_cast<std::uint8_t>(op >> 8));
}
bool checkHeader(const std::uint8_t* p, std::size_t len, std::uint16_t op) {
    if (!p || len < 12) return false;
    if (std::memcmp(p, kId.data(), kId.size()) != 0) return false;
    return (static_cast<std::uint16_t>(p[8]) | static_cast<std::uint16_t>(p[9] << 8)) == op;
}
std::string field(const std::uint8_t* p, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n && p[i] != 0; ++i) s.push_back(p[i] >= 0x20 && p[i] < 0x7F ? static_cast<char>(p[i]) : '?');
    return s;
}
}  // namespace

Result<std::vector<std::uint8_t>> buildDmx(const DmxParams& p, const std::uint8_t* data, std::size_t len) {
    if (!data) return bad("null DMX data");
    if (len < 2 || len > kDmxSlots || (len & 1u)) return bad("ArtDmx length must be even, 2..512");
    if (p.address.wire > kArtNetMaxPortAddress) return bad("Art-Net Port-Address out of range");
    std::vector<std::uint8_t> v;
    v.reserve(kHeaderSize + len);
    putHeader(v, kOpDmx);
    v.push_back(0);                                              // ProtVerHi
    v.push_back(kArtNetProtocolVersion);                         // ProtVerLo = 14
    v.push_back(p.sequence);
    v.push_back(p.physical);
    v.push_back(p.address.subUni());                             // SubUni: SubNet<<4 | Universe
    v.push_back(p.address.net());                                // Net: bits 14..8 of the Port-Address
    v.push_back(static_cast<std::uint8_t>(len >> 8));            // LengthHi (big-endian)
    v.push_back(static_cast<std::uint8_t>(len & 0xFF));          // Length
    v.insert(v.end(), data, data + len);
    return v;
}

Result<ParsedDmx> parseDmx(const std::uint8_t* pkt, std::size_t len) {
    if (!checkHeader(pkt, len, kOpDmx) || len < kHeaderSize) return bad("not an ArtDmx packet");
    if (pkt[10] != 0 || pkt[11] < kArtNetProtocolVersion) return bad("unsupported Art-Net protocol version");
    const std::size_t dl = (static_cast<std::size_t>(pkt[16]) << 8) | pkt[17];
    if (dl < 2 || dl > kDmxSlots || (dl & 1u)) return bad("ArtDmx length must be even, 2..512");
    if (len != kHeaderSize + dl) return bad("ArtDmx length does not match packet size");
    if (pkt[15] > 127) return bad("Net out of range");
    ParsedDmx out;
    out.params.sequence = pkt[12];
    out.params.physical = pkt[13];
    out.params.address.wire = static_cast<std::uint16_t>((pkt[15] << 8) | pkt[14]);
    out.data.assign(pkt + kHeaderSize, pkt + kHeaderSize + dl);
    return out;
}

std::vector<std::uint8_t> buildPoll(const PollParams& p) {
    std::vector<std::uint8_t> v;
    v.reserve(kPollSize);
    putHeader(v, kOpPoll);
    v.push_back(0);
    v.push_back(kArtNetProtocolVersion);
    v.push_back(p.talkToMe);
    v.push_back(p.diagPriority);
    return v;
}

ArtNetPortAddress PollReplyInfo::outputAddress(unsigned i) const {
    return ArtNetPortAddress{static_cast<std::uint16_t>(((netSwitch & 0x7F) << 8) | ((subSwitch & 0x0F) << 4) | (swOut[i & 3] & 0x0F))};
}
ArtNetPortAddress PollReplyInfo::inputAddress(unsigned i) const {
    return ArtNetPortAddress{static_cast<std::uint16_t>(((netSwitch & 0x7F) << 8) | ((subSwitch & 0x0F) << 4) | (swIn[i & 3] & 0x0F))};
}

Result<PollReplyInfo> parsePollReply(const std::uint8_t* p, std::size_t len) {
    if (!checkHeader(p, len, kOpPollReply)) return bad("not an ArtPollReply packet");
    if (len < kPollReplyMinSize) return bad("ArtPollReply too short");
    PollReplyInfo r;
    for (int i = 0; i < 4; ++i) r.ip.b[static_cast<std::size_t>(i)] = p[10 + i];
    r.port = static_cast<std::uint16_t>(p[14] | (p[15] << 8));            // little-endian
    r.versionHi = p[16]; r.versionLo = p[17];
    r.netSwitch = p[18]; r.subSwitch = p[19];
    r.oem = static_cast<std::uint16_t>((p[20] << 8) | p[21]);
    r.status1 = p[23];
    r.estaMan = static_cast<std::uint16_t>(p[24] | (p[25] << 8));         // EstaManLo, EstaManHi
    r.shortName = field(p + 26, 18);
    r.longName = field(p + 44, 64);
    r.nodeReport = field(p + 108, 64);
    r.numPorts = static_cast<std::uint16_t>((p[172] << 8) | p[173]);
    for (std::size_t i = 0; i < 4; ++i) { r.portTypes[i] = p[174 + i]; r.swIn[i] = p[186 + i]; r.swOut[i] = p[190 + i]; }
    r.style = p[200];
    for (std::size_t i = 0; i < 6; ++i) r.mac[i] = p[201 + i];
    if (len >= 212) {
        for (std::size_t i = 0; i < 4; ++i) r.bindIp.b[i] = p[207 + i];
        r.bindIndex = p[211];
        r.hasBind = true;
    }
    return r;
}

}  // namespace ss::lighting::artnet
