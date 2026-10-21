#include "ss/lighting/sacn.hpp"

#include <algorithm>
#include <cstring>
#include "ss/base/utf8.hpp"

namespace ss::lighting::sacn {

namespace {
Error bad(const char* why) { return makeError(ErrorCategory::InvalidArgument, why); }
constexpr std::uint8_t kAcnId[12] = {0x41, 0x53, 0x43, 0x2d, 0x45, 0x31, 0x2e, 0x31, 0x37, 0x00, 0x00, 0x00};  // "ASC-E1.17"
void put16(std::uint8_t* p, std::uint16_t v) { p[0] = static_cast<std::uint8_t>(v >> 8); p[1] = static_cast<std::uint8_t>(v & 0xFF); }
void put32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i)); }
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t get32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }
// Flags (0x7) in the top nibble, PDU length (from the flags field itself to the end of the PDU) in 12 bits.
std::uint16_t flagsLen(std::size_t pduLen) { return static_cast<std::uint16_t>(0x7000u | (pduLen & 0x0FFFu)); }
}  // namespace

std::string truncateSourceName(const std::string& s) {
    if (s.size() <= 63) return s;
    std::size_t n = 63;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // back up to a lead byte
    return s.substr(0, n);
}

Result<std::vector<std::uint8_t>> buildData(const DataParams& p, const std::uint8_t* data, std::size_t len) {
    if (!data) return bad("null DMX data");
    if (len < 1 || len > kDmxSlots) return bad("sACN data length must be 1..512");
    if (p.universe.value < kSacnMinUniverse || p.universe.value > kSacnMaxUniverse) return bad("sACN universe out of range 1..63999");
    if (p.priority > 200) return bad("sACN priority must be 0..200");
    bool allZero = true;
    for (auto b : p.cid) if (b) { allZero = false; break; }
    if (allZero) return bad("sACN CID must not be all zero");
    if (!utf8::isValid(p.sourceName)) return bad("source name must be UTF-8");

    const std::size_t total = kDataOffset + len;
    std::vector<std::uint8_t> v(total, 0);
    std::uint8_t* b = v.data();
    put16(b + 0, 0x0010);                          // preamble size
    put16(b + 2, 0x0000);                          // postamble size
    std::memcpy(b + 4, kAcnId, 12);
    put16(b + 16, flagsLen(total - 16));           // root PDU: from offset 16
    put32(b + 18, 0x00000004);                     // VECTOR_ROOT_E131_DATA
    std::memcpy(b + 22, p.cid.data(), 16);
    put16(b + 38, flagsLen(total - 38));           // framing PDU: from offset 38
    put32(b + 40, 0x00000002);                     // VECTOR_E131_DATA_PACKET
    const std::string name = truncateSourceName(p.sourceName);
    std::memcpy(b + 44, name.data(), name.size()); // remainder already zero: NUL padded to 64 bytes
    b[108] = p.priority;
    put16(b + 109, 0);                             // synchronization address: none
    b[111] = p.sequence;
    b[112] = static_cast<std::uint8_t>((p.preview ? kOptPreview : 0) | (p.streamTerminated ? kOptStreamTerminated : 0));
    put16(b + 113, p.universe.value);
    put16(b + 115, flagsLen(total - 115));         // DMP PDU: from offset 115
    b[117] = 0x02;                                 // VECTOR_DMP_SET_PROPERTY
    b[118] = 0xA1;                                 // address type & data type
    put16(b + 119, 0x0000);                        // first property address
    put16(b + 121, 0x0001);                        // address increment
    put16(b + 123, static_cast<std::uint16_t>(len + 1));   // property value count (start code + slots)
    b[125] = 0x00;                                 // DMX512-A NULL start code
    std::memcpy(b + kDataOffset, data, len);
    return v;
}

Result<ParsedData> parseData(const std::uint8_t* p, std::size_t len) {
    if (!p || len < kDataOffset + 1 || len > kMaxPacketSize) return bad("sACN packet size out of range");
    if (get16(p) != 0x0010 || get16(p + 2) != 0 || std::memcmp(p + 4, kAcnId, 12) != 0) return bad("bad ACN preamble");
    if (get16(p + 16) != flagsLen(len - 16) || get16(p + 38) != flagsLen(len - 38) || get16(p + 115) != flagsLen(len - 115))
        return bad("bad flags/length");
    if (get32(p + 18) != 4 || get32(p + 40) != 2 || p[117] != 2 || p[118] != 0xA1 || get16(p + 119) != 0 || get16(p + 121) != 1)
        return bad("bad vectors or DMP header");
    const std::size_t count = get16(p + 123);
    if (count < 2 || count > 513 || kDataOffset - 1 + count != len) return bad("property count does not match packet");
    if (p[125] != 0) return bad("non-zero start code");
    ParsedData out;
    std::memcpy(out.params.cid.data(), p + 22, 16);
    std::size_t nl = 0; while (nl < 64 && p[44 + nl]) ++nl;
    out.params.sourceName.assign(reinterpret_cast<const char*>(p + 44), nl);
    out.params.priority = p[108];
    out.syncAddress = get16(p + 109);
    out.params.sequence = p[111];
    out.options = p[112];
    out.params.preview = (p[112] & kOptPreview) != 0;
    out.params.streamTerminated = (p[112] & kOptStreamTerminated) != 0;
    out.params.universe.value = get16(p + 113);
    if (out.params.universe.value < kSacnMinUniverse || out.params.universe.value > kSacnMaxUniverse) return bad("universe out of range");
    out.data.assign(p + kDataOffset, p + kDataOffset + (count - 1));
    return out;
}

}  // namespace ss::lighting::sacn
