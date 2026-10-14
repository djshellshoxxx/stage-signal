#include "lighting/artnet.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace ss { namespace artnet {

static const uint8_t kId[8] = {'A','r','t','-','N','e','t',0};

std::string formatUniverse(uint16_t pa) {
  char b[64];
  std::snprintf(b, sizeof b, "%u:%u:%u (port-address %u)", (unsigned)((pa >> 8) & 0x7F), (unsigned)((pa >> 4) & 0xF), (unsigned)(pa & 0xF), (unsigned)pa);
  return b;
}

size_t buildArtDmx(const DmxParams& p, const uint8_t* data, size_t len, uint8_t* out, size_t cap) {
  if (!validPortAddress(p.portAddress) || len < 2 || len > kMaxDmxSlots || (len & 1) || !data || !out) return 0;
  if (cap < kDmxHeaderSize + len) return 0;
  std::memcpy(out, kId, 8);
  out[8] = (uint8_t)(kOpDmx & 0xFF); out[9] = (uint8_t)(kOpDmx >> 8);
  out[10] = 0; out[11] = kProtVerLo;
  out[12] = p.sequence; out[13] = p.physical;
  out[14] = subUniOf(p.portAddress); out[15] = netOf(p.portAddress);
  out[16] = (uint8_t)(len >> 8); out[17] = (uint8_t)(len & 0xFF);
  std::memcpy(out + kDmxHeaderSize, data, len);
  return kDmxHeaderSize + len;
}

static bool checkHeader(const uint8_t* in, size_t n, uint16_t op) {
  if (!in || n < 12 || std::memcmp(in, kId, 8) != 0) return false;
  return (uint16_t)(in[8] | (in[9] << 8)) == op;
}

bool parseArtDmx(const uint8_t* in, size_t n, ArtDmxPacket& o) {
  if (n < kDmxHeaderSize || !checkHeader(in, n, kOpDmx)) return false;
  if (in[10] != 0 || in[11] < kProtVerLo) return false;
  size_t len = ((size_t)in[16] << 8) | in[17];
  if (len < 2 || len > kMaxDmxSlots || (len & 1) || n < kDmxHeaderSize + len) return false;
  if (in[15] & 0x80) return false;
  o.p.sequence = in[12]; o.p.physical = in[13];
  o.p.portAddress = (uint16_t)(((in[15] & 0x7F) << 8) | in[14]);
  o.length = (uint16_t)len; o.data = in + kDmxHeaderSize;
  return true;
}

size_t buildArtPoll(uint8_t flags, uint8_t diag, uint8_t* out, size_t cap) {
  if (!out || cap < kPollSize) return 0;
  std::memcpy(out, kId, 8);
  out[8] = (uint8_t)(kOpPoll & 0xFF); out[9] = (uint8_t)(kOpPoll >> 8);
  out[10] = 0; out[11] = kProtVerLo; out[12] = flags; out[13] = diag;
  return kPollSize;
}

bool parseArtPoll(const uint8_t* in, size_t n, uint8_t& flags, uint8_t& diag) {
  if (n < kPollSize || !checkHeader(in, n, kOpPoll) || in[10] != 0 || in[11] < kProtVerLo) return false;
  flags = in[12]; diag = in[13]; return true;
}

static std::string cstr(const uint8_t* p, size_t max) {
  size_t l = 0; while (l < max && p[l]) ++l; return std::string((const char*)p, l);
}

std::string PollReply::ipString() const {
  char b[24]; std::snprintf(b, sizeof b, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]); return b;
}

bool parsePollReply(const uint8_t* in, size_t n, PollReply& r) {
  if (n < kPollReplyMinSize || !checkHeader(in, n, kOpPollReply)) return false;
  std::memcpy(r.ip, in + 10, 4);
  r.port = (uint16_t)(in[14] | (in[15] << 8));
  r.firmware = (uint16_t)((in[16] << 8) | in[17]);
  r.netSwitch = in[18]; r.subSwitch = in[19];
  r.oem = (uint16_t)((in[20] << 8) | in[21]);
  r.status1 = in[23];
  r.estaMan = (uint16_t)(in[24] | (in[25] << 8));
  r.shortName = cstr(in + 26, 18); r.longName = cstr(in + 44, 64); r.nodeReport = cstr(in + 108, 64);
  r.numPorts = (uint16_t)((in[172] << 8) | in[173]);
  std::memcpy(r.portTypes, in + 174, 4); std::memcpy(r.swIn, in + 186, 4); std::memcpy(r.swOut, in + 190, 4);
  r.style = in[200]; std::memcpy(r.mac, in + 201, 6);
  return true;
}

bool isBroadcastAddress(const std::string& h) {
  if (h == "255.255.255.255") return true;
  unsigned a, b, c, d; char tail;
  if (std::sscanf(h.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
  return a < 256 && b < 256 && c < 256 && d == 255;
}

Status sendArtPoll(IDatagramSink& sink, const std::string& host, bool allowBroadcastDiscovery) {
  if (host.empty()) return Status::Fail(Err::InvalidArgument, "ArtPoll destination required");
  if (isBroadcastAddress(host) && !allowBroadcastDiscovery)
    return Status::Fail(Err::PermissionDenied, "Art-Net broadcast discovery is opt-in; unicast to a known node by default");
  uint8_t b[kPollSize]; size_t n = buildArtPoll(0x00, 0, b, sizeof b);
  return sink.sendTo(host, kUdpPort, b, n);
}

}} // namespace ss::artnet
