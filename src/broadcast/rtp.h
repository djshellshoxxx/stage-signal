// MOD-12 plain RTP/UDP sender (REQ-B07): MJPEG over RTP (RFC 2435, PT 26), PCM L16 (RFC 3551 / RFC 3550 timing), RTCP SR+SDES.
// UNENCRYPTED by design (RTP/AVP; no SRTP) and labelled as such everywhere. RTP has no delivery acknowledgement here (no RTCP
// RR parsing): "Live" for an RTP session means datagrams left the socket, not that anyone receives them.
// Sockets are abstract (IUdpSocket): a Windows adapter is supplied elsewhere; tests use FakeUdpSocket.
#pragma once
#include "broadcast/transport.h"
namespace ss {

struct IUdpSocket {
  virtual ~IUdpSocket() = default;
  virtual Status open(const std::string& nicAddr /*"" = default*/, int ttl) = 0;
  virtual Status sendTo(const std::string& host, uint16_t port, const uint8_t* d, size_t n) = 0;
  virtual void close() = 0;
};
struct FakeUdpSocket : IUdpSocket {
  struct Datagram { std::string host; uint16_t port = 0; std::vector<uint8_t> data; };
  std::vector<Datagram> sent; bool opened = false; int ttl = 0; int failAfter = -1; std::string openFail;
  Status open(const std::string&, int t) override { if (!openFail.empty()) return Status::Fail(Err::TransportFailure, openFail); opened = true; ttl = t; return Status::Ok(); }
  Status sendTo(const std::string& h, uint16_t p, const uint8_t* d, size_t n) override {
    if (!opened) return Status::Fail(Err::TransportFailure, "socket not open");
    if (failAfter >= 0 && (int)sent.size() >= failAfter) return Status::Fail(Err::TransportFailure, "network unreachable");
    sent.push_back({h, p, std::vector<uint8_t>(d, d + n)}); return Status::Ok(); }
  void close() override { opened = false; }
};

namespace rtp {
using Bytes = std::vector<uint8_t>;
constexpr uint8_t kPtJpeg = 26;
constexpr uint32_t kVideoClock = 90000;
Bytes header(uint8_t pt, bool marker, uint16_t seq, uint32_t ts, uint32_t ssrc);   // 12-byte fixed header, V=2

struct JpegInfo { int type = 1; int width = 0, height = 0; Bytes qtables; size_t scanOff = 0, scanLen = 0; };
// Extract what RFC 2435 needs from a baseline JFIF stream (4:2:0 -> type 1, 4:2:2 -> type 0, 8-bit tables, no restart markers).
Status parseJpeg(const uint8_t* d, size_t n, JpegInfo& out);
// One RTP packet per element; marker bit set on the last. Q=255 (tables in-band on the first fragment of every frame).
std::vector<Bytes> packetizeJpeg(const JpegInfo& j, const uint8_t* jpeg, uint32_t ts, uint32_t ssrc, uint16_t& seq, size_t maxPayload);
// L16 big-endian packets from float samples (clamped, round-half-away, +-32767); <= maxFramesPerPacket frames each.
size_t l16FramesPerPacket(size_t maxPayload, uint32_t channels, uint32_t rate);   // min(payload budget, 5 ms) => a=ptime:5
std::vector<Bytes> packetizeL16(const float* pcm, size_t frames, uint32_t channels, uint8_t pt, uint32_t ts, uint32_t ssrc, uint16_t& seq, size_t maxFramesPerPacket, bool firstMarker);
// RTCP compound: SR (PT 200) + SDES(CNAME) (PT 202), RFC 3550 6.4.1 / 6.5.
Bytes senderReport(uint32_t ssrc, uint32_t ntpSec, uint32_t ntpFrac, uint32_t rtpTs, uint32_t pktCount, uint32_t octetCount, const std::string& cname);
void unixMsToNtp(int64_t unixMs, uint32_t& sec, uint32_t& frac);
} // namespace rtp

struct RtpSenderConfig {
  std::string host; uint16_t videoPort = 5004, audioPort = 5006;       // RTCP on port+1
  int ttl = 1; size_t maxPayload = 1200; uint32_t seed = 1;            // seed => deterministic SSRC/seq/timestamp bases
  uint32_t audioRate = 48000, audioChannels = 2; uint8_t audioPt = 97; int64_t rtcpIntervalMs = 5000;
  std::string cname = "stagesignal"; IClock* clock = nullptr;
};
struct RtpStats { uint64_t videoPackets = 0, audioPackets = 0, videoOctets = 0, audioOctets = 0, rtcpPackets = 0, videoFrames = 0, sendErrors = 0; };

class RtpSender {
 public:
  Status open(std::unique_ptr<IUdpSocket> sock, const RtpSenderConfig& c);
  Status sendVideoJpeg(Ns pts, const uint8_t* jpeg, size_t n);
  Status sendAudioF32(Ns pts, const float* interleaved, size_t frames);
  Status service();                       // emits RTCP SR+SDES when due (call regularly, e.g. every tick)
  void close();
  const RtpStats& stats() const { return st_; }
  uint32_t videoSsrc() const { return vSsrc_; } uint32_t audioSsrc() const { return aSsrc_; }
 private:
  Status sendTo(uint16_t port, const rtp::Bytes& b);
  Status sendRtcp(bool video);
  std::unique_ptr<IUdpSocket> sock_; RtpSenderConfig c_; RtpStats st_; bool open_ = false;
  uint32_t vSsrc_ = 0, aSsrc_ = 0, vTsBase_ = 0, aTsBase_ = 0; uint16_t vSeq_ = 0, aSeq_ = 0;
  bool haveV_ = false, haveA_ = false; Ns vLastPts_ = 0, aLastEndPts_ = 0; uint32_t vLastTs_ = 0, aLastEndTs_ = 0; uint64_t aSamples_ = 0; Ns aFirstPts_ = 0;
  uint32_t vPk_ = 0, vOct_ = 0, aPk_ = 0, aOct_ = 0; int64_t lastRtcpMs_ = -1;
};

// SDP the receiver needs (REQ-B07): matching payload types, ports, clock rates, and an explicit encryption label.
std::string buildSdp(const BroadcastSettings& s, const RtpSenderConfig& c);

// ITransport over RtpSender (works with any IUdpSocket). MediaPacket mapping: Video/Mjpeg => RFC 2435, Audio/PcmF32 => L16.
std::unique_ptr<ITransport> makeRtpTransport(TransportFactory::MakeSocket sockets, IClock* clock, uint32_t seed = 0);

} // namespace ss
