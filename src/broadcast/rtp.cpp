#include "broadcast/rtp.h"
#include "broadcast/redact.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <algorithm>
namespace ss {
namespace rtp {

static void p16(Bytes& b, uint32_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
static void p32(Bytes& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); }

Bytes header(uint8_t pt, bool marker, uint16_t seq, uint32_t ts, uint32_t ssrc) {
  Bytes b; b.reserve(12); b.push_back(0x80); b.push_back((uint8_t)((marker ? 0x80 : 0) | (pt & 0x7f))); p16(b, seq); p32(b, ts); p32(b, ssrc); return b;
}

Status parseJpeg(const uint8_t* d, size_t n, JpegInfo& o) {
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return Status::Fail(Err::InvalidArgument, "not a JPEG (no SOI)");
  JpegInfo j; Bytes qt[4]; bool haveQ[4] = {false, false, false, false}; bool sof = false; int hs[3] = {0, 0, 0}, ncomp = 0;
  size_t p = 2;
  while (p + 4 <= n) {
    if (d[p] != 0xFF) return Status::Fail(Err::InvalidArgument, "JPEG marker expected");
    uint8_t m = d[p + 1]; if (m == 0xFF) { ++p; continue; }
    if (m == 0xD9) break;
    size_t len = ((size_t)d[p + 2] << 8) | d[p + 3];
    if (p + 2 + len > n || len < 2) return Status::Fail(Err::InvalidArgument, "truncated JPEG segment");
    const uint8_t* s = d + p + 4; size_t sl = len - 2;
    if (m == 0xDB) {
      size_t k = 0;
      while (k < sl) { uint8_t pq = s[k] >> 4, tq = s[k] & 15; if (pq != 0) return Status::Fail(Err::UnsupportedCapability, "16-bit quantization tables are not carried by RFC 2435 Q=255 here"); if (tq > 3 || k + 65 > sl) return Status::Fail(Err::InvalidArgument, "bad DQT"); qt[tq].assign(s + k + 1, s + k + 65); haveQ[tq] = true; k += 65; }
    } else if (m == 0xC0) {
      if (sl < 6 || s[0] != 8) return Status::Fail(Err::UnsupportedCapability, "only 8-bit baseline JPEG (SOF0) is supported");
      j.height = (s[1] << 8) | s[2]; j.width = (s[3] << 8) | s[4]; ncomp = s[5];
      if (ncomp != 3 || sl < 6 + 9) return Status::Fail(Err::UnsupportedCapability, "RFC 2435 needs 3-component YCbCr");
      for (int c = 0; c < 3; ++c) hs[c] = s[7 + c * 3];
      sof = true;
    } else if (m == 0xC1 || m == 0xC2 || (m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)) return Status::Fail(Err::UnsupportedCapability, "progressive/lossless JPEG cannot be carried by RFC 2435");
    else if (m == 0xDD) return Status::Fail(Err::UnsupportedCapability, "JPEG with restart intervals is not supported by this packetizer");
    else if (m == 0xDA) {
      if (!sof) return Status::Fail(Err::InvalidArgument, "SOS before SOF");
      j.scanOff = p + 2 + len; break;
    }
    p += 2 + len;
  }
  if (!sof || !j.scanOff) return Status::Fail(Err::InvalidArgument, "JPEG has no frame/scan");
  if (!haveQ[0] || !haveQ[1]) return Status::Fail(Err::InvalidArgument, "JPEG lacks two quantization tables");
  if (hs[1] != 0x11 || hs[2] != 0x11) return Status::Fail(Err::UnsupportedCapability, "chroma must be 1x1 sampled");
  if (hs[0] == 0x22) j.type = 1; else if (hs[0] == 0x21) j.type = 0;
  else return Status::Fail(Err::UnsupportedCapability, "4:4:4 JPEG cannot be carried by RFC 2435; encode with quality <= 90 (4:2:0)");
  if (j.width <= 0 || j.height <= 0 || j.width > 2040 || j.height > 2040 || (j.width & 7) || (j.height & 7)) return Status::Fail(Err::UnsupportedCapability, "RFC 2435 needs width/height multiples of 8, at most 2040");
  size_t end = n; if (end >= 2 && d[end - 2] == 0xFF && d[end - 1] == 0xD9) end -= 2;
  if (end <= j.scanOff) return Status::Fail(Err::InvalidArgument, "empty scan");
  j.scanLen = end - j.scanOff; j.qtables = qt[0]; j.qtables.insert(j.qtables.end(), qt[1].begin(), qt[1].end());
  o = j; return Status::Ok();
}

std::vector<Bytes> packetizeJpeg(const JpegInfo& j, const uint8_t* jpeg, uint32_t ts, uint32_t ssrc, uint16_t& seq, size_t maxPayload) {
  std::vector<Bytes> out; size_t off = 0; const uint8_t* scan = jpeg + j.scanOff;
  while (off < j.scanLen) {
    Bytes pk = header(kPtJpeg, false, seq++, ts, ssrc);
    pk.push_back(0); pk.push_back((uint8_t)(off >> 16)); pk.push_back((uint8_t)(off >> 8)); pk.push_back((uint8_t)off);
    pk.push_back((uint8_t)j.type); pk.push_back(255); pk.push_back((uint8_t)(j.width / 8)); pk.push_back((uint8_t)(j.height / 8));
    size_t room = maxPayload > 8 ? maxPayload - 8 : 1;
    if (off == 0) { pk.push_back(0); pk.push_back(0); p16(pk, (uint32_t)j.qtables.size()); pk.insert(pk.end(), j.qtables.begin(), j.qtables.end()); room = room > 4 + j.qtables.size() ? room - 4 - j.qtables.size() : 1; }
    size_t take = std::min(room, j.scanLen - off);
    pk.insert(pk.end(), scan + off, scan + off + take); off += take;
    if (off >= j.scanLen) pk[1] |= 0x80;
    out.push_back(std::move(pk));
  }
  return out;
}

std::vector<Bytes> packetizeL16(const float* pcm, size_t frames, uint32_t ch, uint8_t pt, uint32_t ts, uint32_t ssrc, uint16_t& seq, size_t maxPayload, bool firstMarker) {
  std::vector<Bytes> out; size_t per = std::max<size_t>(1, maxPayload / (2 * ch)); size_t done = 0; bool first = firstMarker;
  while (done < frames) {
    size_t n = std::min(per, frames - done);
    Bytes pk = header(pt, first, seq++, ts + (uint32_t)done, ssrc); first = false;
    for (size_t i = 0; i < n * ch; ++i) {
      float f = pcm[done * ch + i]; if (!(f == f)) f = 0.f; if (f > 1.f) f = 1.f; if (f < -1.f) f = -1.f;
      float s = f * 32767.f; int v = (int)(s >= 0 ? s + 0.5f : s - 0.5f); p16(pk, (uint32_t)(v & 0xFFFF));
    }
    out.push_back(std::move(pk)); done += n;
  }
  return out;
}

Bytes senderReport(uint32_t ssrc, uint32_t ntpSec, uint32_t ntpFrac, uint32_t rtpTs, uint32_t pkts, uint32_t octets, const std::string& cname) {
  Bytes b; b.push_back(0x80); b.push_back(200); p16(b, 6); p32(b, ssrc); p32(b, ntpSec); p32(b, ntpFrac); p32(b, rtpTs); p32(b, pkts); p32(b, octets);
  std::string cn = cname.substr(0, 255);
  Bytes chunk; p32(chunk, ssrc); chunk.push_back(1); chunk.push_back((uint8_t)cn.size()); chunk.insert(chunk.end(), cn.begin(), cn.end()); chunk.push_back(0);
  while (chunk.size() % 4) chunk.push_back(0);
  b.push_back(0x81); b.push_back(202); p16(b, (uint32_t)(chunk.size() / 4)); b.insert(b.end(), chunk.begin(), chunk.end());
  return b;
}
void unixMsToNtp(int64_t ms, uint32_t& sec, uint32_t& frac) {
  uint64_t s = (uint64_t)(ms / 1000) + 2208988800ull; uint64_t rem = (uint64_t)(ms % 1000);
  sec = (uint32_t)s; frac = (uint32_t)((rem << 32) / 1000);
}
} // namespace rtp

// ============================== RtpSender ==============================
static uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }
static uint64_t ptsToSamples(Ns pts, uint32_t rate) { if (pts <= 0) return 0; uint64_t n = (uint64_t)pts; return (n / 1000000000ull) * rate + (n % 1000000000ull) * rate / 1000000000ull; }
static bool isMulticastHost(const std::string& h) {
  int a = 0; size_t i = 0; while (i < h.size() && std::isdigit((unsigned char)h[i])) { a = a * 10 + (h[i] - '0'); ++i; }
  if (i > 0 && i < h.size() && h[i] == '.') return a >= 224 && a <= 239;
  return h.size() > 2 && (h[0] == 'f' || h[0] == 'F') && (h[1] == 'f' || h[1] == 'F') && h.find(':') != std::string::npos;
}

Status RtpSender::open(std::unique_ptr<IUdpSocket> sock, const RtpSenderConfig& c) {
  if (!sock) return Status::Fail(Err::UnsupportedCapability, "No UDP socket implementation is available (the Windows socket adapter registers one).");
  if (!c.clock) return Status::Fail(Err::InternalFault, "RtpSender needs a clock");
  c_ = c; Status s = sock->open("", c.ttl); if (!s.ok()) return Status::Fail(Err::TransportFailure, cast::redactText(s.detail));
  sock_ = std::move(sock); uint32_t r = c.seed ? c.seed : 1;
  vSsrc_ = lcg(r); aSsrc_ = lcg(r); if (aSsrc_ == vSsrc_) ++aSsrc_;
  vTsBase_ = lcg(r); aTsBase_ = lcg(r); vSeq_ = (uint16_t)(lcg(r) >> 8); aSeq_ = (uint16_t)(lcg(r) >> 8);
  st_ = RtpStats(); haveV_ = haveA_ = false; aSamples_ = 0; lastRtcpMs_ = -1; vPk_ = vOct_ = aPk_ = aOct_ = 0; open_ = true; return Status::Ok();
}
void RtpSender::close() { if (sock_) sock_->close(); open_ = false; }
Status RtpSender::sendTo(uint16_t port, const rtp::Bytes& b) {
  Status s = sock_->sendTo(c_.host, port, b.data(), b.size());
  if (!s.ok()) { ++st_.sendErrors; return Status::Fail(Err::TransportFailure, cast::redactText(s.detail)); }
  return Status::Ok();
}
Status RtpSender::sendVideoJpeg(Ns pts, const uint8_t* jpeg, size_t n) {
  if (!open_) return Status::Fail(Err::InternalFault, "RTP sender not open");
  rtp::JpegInfo ji; Status ps = rtp::parseJpeg(jpeg, n, ji); if (!ps.ok()) return ps;
  uint32_t ts = vTsBase_ + (uint32_t)((uint64_t)(pts < 0 ? 0 : pts) / 1000 * rtp::kVideoClock / 1000000ull);   // pts ns -> 90 kHz ticks
  for (auto& pk : rtp::packetizeJpeg(ji, jpeg, ts, vSsrc_, vSeq_, c_.maxPayload)) {
    Status s = sendTo(c_.videoPort, pk); if (!s.ok()) return s;
    ++st_.videoPackets; ++vPk_; vOct_ += (uint32_t)(pk.size() - 12); st_.videoOctets += pk.size() - 12;
  }
  ++st_.videoFrames; haveV_ = true; vLastPts_ = pts; vLastTs_ = ts; return Status::Ok();
}
Status RtpSender::sendAudioF32(Ns pts, const float* pcm, size_t frames) {
  if (!open_) return Status::Fail(Err::InternalFault, "RTP sender not open");
  if (!frames) return Status::Ok();
  if (!haveA_) { aFirstPts_ = pts; aSamples_ = 0; }
  uint32_t ts0 = aTsBase_ + (uint32_t)(ptsToSamples(aFirstPts_, c_.audioRate) + aSamples_);
  bool firstMarker = !haveA_;
  for (auto& pk : rtp::packetizeL16(pcm, frames, c_.audioChannels, c_.audioPt, ts0, aSsrc_, aSeq_, c_.maxPayload, firstMarker)) {
    Status s = sendTo(c_.audioPort, pk); if (!s.ok()) return s;
    ++st_.audioPackets; ++aPk_; aOct_ += (uint32_t)(pk.size() - 12); st_.audioOctets += pk.size() - 12;
  }
  haveA_ = true; aSamples_ += frames;
  aLastEndTs_ = ts0 + (uint32_t)frames; aLastEndPts_ = pts + (Ns)((long double)frames * 1e9L / c_.audioRate);
  return Status::Ok();
}
Status RtpSender::sendRtcp(bool video) {
  Ns nowNs = c_.clock->nowNs(); int64_t wall = c_.clock->wallUnixMs();
  uint32_t sec, frac; rtp::unixMsToNtp(wall, sec, frac);
  uint32_t ts, pk, oct, ssrc; uint16_t port;
  if (video) { ts = vLastTs_ + (uint32_t)((uint64_t)((nowNs - vLastPts_) < 0 ? 0 : (nowNs - vLastPts_)) / 1000 * rtp::kVideoClock / 1000000ull); pk = vPk_; oct = vOct_; ssrc = vSsrc_; port = (uint16_t)(c_.videoPort + 1); }
  else { Ns dt = nowNs - aLastEndPts_; if (dt < 0) dt = 0; ts = aLastEndTs_ + (uint32_t)((long double)dt * c_.audioRate / 1e9L); pk = aPk_; oct = aOct_; ssrc = aSsrc_; port = (uint16_t)(c_.audioPort + 1); }
  rtp::Bytes sr = rtp::senderReport(ssrc, sec, frac, ts, pk, oct, c_.cname);
  Status s = sendTo(port, sr); if (s.ok()) ++st_.rtcpPackets; return s;
}
Status RtpSender::service() {
  if (!open_) return Status::Ok();
  int64_t wall = c_.clock->wallUnixMs();
  if (lastRtcpMs_ >= 0 && wall - lastRtcpMs_ < c_.rtcpIntervalMs) return Status::Ok();
  if (!haveV_ && !haveA_) return Status::Ok();
  lastRtcpMs_ = wall; Status r = Status::Ok();
  if (haveV_) { Status s = sendRtcp(true); if (!s.ok()) r = s; }
  if (haveA_) { Status s = sendRtcp(false); if (!s.ok()) r = s; }
  return r;
}

std::string buildSdp(const BroadcastSettings& s, const RtpSenderConfig& c) {
  auto clean = [](std::string t) { std::string o; for (char ch : t) if ((unsigned char)ch >= 0x20 && (unsigned char)ch < 0x7f) o.push_back(ch); return o; };
  bool v6 = c.host.find(':') != std::string::npos;
  std::string t;
  t += "v=0\r\n";
  t += std::string("o=- 0 0 IN ") + (v6 ? "IP6 ::" : "IP4 0.0.0.0") + "\r\n";
  t += "s=" + clean(s.endpoint.name.empty() ? std::string("StageSignal") : "StageSignal " + s.endpoint.name) + "\r\n";
  t += "i=UNENCRYPTED RTP/AVP contribution from StageSignal (no SRTP)\r\n";
  t += std::string("c=IN ") + (v6 ? "IP6 " : "IP4 ") + c.host + (isMulticastHost(c.host) ? "/" + std::to_string(c.ttl) : std::string()) + "\r\n";
  t += "t=0 0\r\n";
  t += "a=recvonly\r\n";
  t += "a=x-stagesignal-encryption:none\r\n";
  t += "m=video " + std::to_string(c.videoPort) + " RTP/AVP 26\r\n";
  t += "a=rtpmap:26 JPEG/90000\r\n";
  t += "a=framerate:" + std::to_string(s.fps) + "\r\n";
  t += "a=x-dimensions:" + std::to_string(s.width) + "," + std::to_string(s.height) + "\r\n";
  t += "m=audio " + std::to_string(c.audioPort) + " RTP/AVP " + std::to_string((int)c.audioPt) + "\r\n";
  t += "a=rtpmap:" + std::to_string((int)c.audioPt) + " L16/" + std::to_string(c.audioRate) + "/" + std::to_string(c.audioChannels) + "\r\n";
  t += "a=ptime:" + std::to_string((int)(1000.0 * (double)std::max<size_t>(1, c.maxPayload / (2 * c.audioChannels)) / c.audioRate + 0.5)) + "\r\n";
  return t;
}

// ============================== RtpTransport ==============================
namespace {
class RtpTransport : public ITransport {
 public:
  RtpTransport(TransportFactory::MakeSocket f, IClock* clock, uint32_t seed) : mk_(std::move(f)), clock_(clock), seed_(seed) {}
  Status connect(const TransportParams& p, std::function<void(const TransportEvent&)> cb) override {
    closed_ = false;
    std::unique_ptr<IUdpSocket> sock = mk_ ? mk_() : nullptr;
    RtpSenderConfig c; c.host = p.settings.endpoint.host; c.videoPort = p.settings.endpoint.port; c.audioPort = (uint16_t)(p.settings.endpoint.port + 2);
    c.ttl = p.settings.rtpTtl; c.maxPayload = (size_t)p.settings.rtpMtu; c.audioRate = p.settings.audioRate; c.audioChannels = p.settings.audioChannels; c.clock = clock_; c.seed = seed_ ? seed_ : 0x5eed1234u;
    Status s = sender_.open(std::move(sock), c);
    TransportEvent e;
    if (!s.ok()) { e.type = TransportEvent::Type::Disconnected; e.cls = s.code == Err::UnsupportedCapability ? TransportErrorClass::InvalidConfig : TransportErrorClass::Network; e.detail = s.detail; }
    else e.type = TransportEvent::Type::Connected;      // UDP: socket ready; there is no handshake and no receiver acknowledgement
    if (cb) cb(e);
    return Status::Ok();
  }
  Status send(const MediaPacket& pk) override {
    if (closed_) return Status::Fail(Err::TransportFailure, "closed");
    Status s;
    if (pk.kind == PacketKind::Headers) return Status::Ok();
    if (pk.kind == PacketKind::Video && pk.codec == PacketCodec::Mjpeg) s = sender_.sendVideoJpeg(pk.pts, pk.data.data(), pk.data.size());
    else if (pk.kind == PacketKind::Audio && pk.codec == PacketCodec::PcmF32) s = sender_.sendAudioF32(pk.pts, (const float*)pk.data.data(), pk.frames);
    else return Status::Fail(Err::UnsupportedCapability, "RTP transport carries MJPEG video and PCM audio only");
    if (s.ok()) { ++st_.packetsSent; st_.bytesSent += pk.data.size(); sender_.service(); } else ++st_.packetsRejected;
    return s;
  }
  void close() override { closed_ = true; sender_.close(); }
  TransportStats stats() const override { return st_; }
  std::string describe() const override { return "rtp-udp (unencrypted)"; }
 private:
  TransportFactory::MakeSocket mk_; IClock* clock_; uint32_t seed_; RtpSender sender_; TransportStats st_; bool closed_ = true;
};
} // namespace
std::unique_ptr<ITransport> makeRtpTransport(TransportFactory::MakeSocket s, IClock* clock, uint32_t seed) { return std::unique_ptr<ITransport>(new RtpTransport(std::move(s), clock, seed)); }

} // namespace ss
