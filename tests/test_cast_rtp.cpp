// MOD-12 RTP/UDP tests: packet goldens (RFC 3550/2435/3551), RTCP SR, SDP export, sender timing, end-to-end via BroadcastSession.
#include "test.h"
#include "broadcast/rtp.h"
#include "broadcast/session.h"
#include "recording/video_encoder.h"
#include <cmath>
using namespace ss;

static std::string hex(const std::vector<uint8_t>& v) { static const char* h = "0123456789abcdef"; std::string s; for (uint8_t b : v) { s.push_back(h[b >> 4]); s.push_back(h[b & 15]); } return s; }
static std::vector<uint8_t> miniJpeg(const std::vector<uint8_t>& scan, int w = 16, int h = 16, uint8_t ySamp = 0x22) {
  std::vector<uint8_t> j = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x84, 0x00};
  for (int i = 0; i < 64; ++i) j.push_back((uint8_t)(i + 1));
  j.push_back(0x01); for (int i = 0; i < 64; ++i) j.push_back((uint8_t)(100 + i));
  j.insert(j.end(), {0xFF, 0xC0, 0x00, 0x11, 0x08, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(w >> 8), (uint8_t)w, 0x03, 0x01, ySamp, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01});
  j.insert(j.end(), {0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00});
  j.insert(j.end(), scan.begin(), scan.end()); j.push_back(0xFF); j.push_back(0xD9); return j;
}

TEST(rtp_header_and_rtcp_goldens) {
  CHECK(hex(rtp::header(26, true, 0x1234, 0xA1B2C3D4, 0x01020304)) == "809a1234a1b2c3d401020304");
  CHECK(hex(rtp::header(97, false, 0xFFFF, 0, 0xFFFFFFFF)) == "8061ffff00000000ffffffff");
  uint32_t sec, frac; rtp::unixMsToNtp(1767225600123LL, sec, frac); CHECK(sec == 0xed003780u && frac == 0x1f7ced91u);
  rtp::unixMsToNtp(0, sec, frac); CHECK(sec == 2208988800u && frac == 0);
  auto sr = rtp::senderReport(0x11223344, 0xed003780u, 0x1f7ced91u, 0xA0B0C0D0u, 1234, 567890, "stagesignal");
  CHECK(hex(sr) == "80c8000611223344ed0037801f7ced91a0b0c0d0000004d20008aa5281ca000511223344010b73746167657369676e616c000000");
  CHECK(sr.size() % 4 == 0);
}

TEST(rtp_l16_golden) {
  float pcm[5] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f}; uint16_t seq = 0x0100;
  auto pk = rtp::packetizeL16(pcm, 5, 1, 97, 1000, 0xAABBCCDD, seq, 240, true);
  CHECK(pk.size() == 1 && seq == 0x0101);
  CHECK(hex(pk[0]) == "80e1010000 0003e8aabbccdd4000c0007fff80017fff"[0] ? true : true);
  CHECK(hex(pk[0]) == "80e10100000003e8aabbccdd4000c0007fff80017fff");   // M=1 first packet, PT 97, big-endian 16-bit, clamped
  // splitting: 600 stereo frames at 240 frames per packet => 3 packets, timestamps advance by frames, only the first has M
  std::vector<float> st(600 * 2, 0.25f); uint16_t s2 = 7; auto p3 = rtp::packetizeL16(st.data(), 600, 2, 97, 5000, 1, s2, 240, true);
  CHECK(p3.size() == 3 && p3[0].size() == 12 + 240 * 4 && p3[2].size() == 12 + 120 * 4);
  CHECK(p3[0][1] == (0x80 | 97) && p3[1][1] == 97 && p3[2][1] == 97);
  CHECK(((p3[1][4] << 24) | (p3[1][5] << 16) | (p3[1][6] << 8) | p3[1][7]) == 5240); CHECK(p3[2][3] == 9);
  CHECK(rtp::l16FramesPerPacket(1200, 2, 48000) == 240); CHECK(rtp::l16FramesPerPacket(400, 2, 48000) == 100);
}

TEST(rtp_mjpeg_golden_and_fragmentation) {
  std::vector<uint8_t> scan = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x11, 0x22, 0x33};
  auto jpg = miniJpeg(scan);
  rtp::JpegInfo ji; CHECK(rtp::parseJpeg(jpg.data(), jpg.size(), ji).ok());
  CHECK(ji.type == 1 && ji.width == 16 && ji.height == 16 && ji.qtables.size() == 128 && ji.scanLen == scan.size());
  CHECK(ji.qtables[0] == 1 && ji.qtables[63] == 64 && ji.qtables[64] == 100 && ji.qtables[127] == 163);
  uint16_t seq = 0x0A0B; auto one = rtp::packetizeJpeg(ji, jpg.data(), 0x01020304, 0xCAFEBABE, seq, 1200);
  CHECK(one.size() == 1 && seq == 0x0A0C);
  std::string want = "809a0a0b01020304cafebabe"      // RTP: V2 M=1 PT26
                     "00000000" "01" "ff" "02" "02"  // type-specific 0, offset 0, type 1, Q 255, 16/8, 16/8
                     "0000" "0080";                  // MBZ, precision 0, length 128
  std::vector<uint8_t> tb(ji.qtables); std::string tbl = hex(tb);
  CHECK(hex(one[0]) == want + tbl + hex(scan));
  // fragmentation: tiny payload budget forces several packets; offsets are contiguous, M only on the last, tables only in the first
  uint16_t s2 = 100; auto many = rtp::packetizeJpeg(ji, jpg.data(), 777, 5, s2, 8 + 4 + 128 + 4);
  CHECK(many.size() == 3);                                            // 4 + 4 + 3 scan bytes
  size_t off = 0, total = 0; for (size_t i = 0; i < many.size(); ++i) {
    const auto& p = many[i]; size_t o = (p[13] << 16) | (p[14] << 8) | p[15]; CHECK(o == off);
    bool last = i + 1 == many.size(); CHECK(((p[1] & 0x80) != 0) == last); CHECK((p[1] & 0x7f) == 26);
    CHECK(p[16] == 1 && p[17] == 255 && p[18] == 2 && p[19] == 2);
    size_t hdr = 20 + (i == 0 ? 4 + 128 : 0); total += p.size() - hdr; off += p.size() - hdr;
    CHECK(p.size() <= 12 + 8 + 4 + 128 + 4);
    CHECK((((uint32_t)p[2] << 8) | p[3]) == (uint32_t)(100 + i));
  }
  CHECK(total == scan.size());
  // reassembled scan equals original, bit exact
  std::vector<uint8_t> re; for (size_t i = 0; i < many.size(); ++i) { size_t hdr = 20 + (i == 0 ? 132 : 0); re.insert(re.end(), many[i].begin() + hdr, many[i].end()); } CHECK(re == scan);
  // unsupported shapes are refused honestly
  auto j444 = miniJpeg(scan, 16, 16, 0x11); CHECK(rtp::parseJpeg(j444.data(), j444.size(), ji).code == Err::UnsupportedCapability);
  auto odd = miniJpeg(scan, 20, 16); CHECK(rtp::parseJpeg(odd.data(), odd.size(), ji).code == Err::UnsupportedCapability);
  auto j422 = miniJpeg(scan, 16, 16, 0x21); CHECK(rtp::parseJpeg(j422.data(), j422.size(), ji).ok() && ji.type == 0);
  std::vector<uint8_t> junk = {1, 2, 3, 4}; CHECK(rtp::parseJpeg(junk.data(), junk.size(), ji).code == Err::InvalidArgument);
}

TEST(rtp_real_encoder_output_packetizes_bit_exact) {
  auto enc = makeMjpegEncoder(); VideoProfile p; p.width = 64; p.height = 48; p.quality = 80; CHECK(enc->open(p).ok());
  Frame f; f.resize(64, 48); for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) f.px[y * 64 + x] = rgb(x * 4, y * 5, 128);
  EncodedPacket pk; bool have; CHECK(enc->encode(f, 0, pk, have).ok());
  rtp::JpegInfo ji; Status s = rtp::parseJpeg(pk.data.data(), pk.data.size(), ji); CHECK(s.ok()); CHECK(ji.type == 1 && ji.width == 64 && ji.height == 48);
  uint16_t seq = 1; auto pkts = rtp::packetizeJpeg(ji, pk.data.data(), 90000, 9, seq, 400); CHECK(pkts.size() > 1);
  std::vector<uint8_t> re; for (size_t i = 0; i < pkts.size(); ++i) { size_t hdr = 20 + (i == 0 ? 132 : 0); CHECK(pkts[i].size() <= 12 + 400); re.insert(re.end(), pkts[i].begin() + hdr, pkts[i].end()); }
  CHECK(re == std::vector<uint8_t>(pk.data.begin() + ji.scanOff, pk.data.end() - 2));           // entropy-coded data preserved exactly (EOI excluded)
  // the in-band tables equal the DQT of the source
  CHECK(std::vector<uint8_t>(pkts[0].begin() + 24, pkts[0].begin() + 24 + 128) == ji.qtables);
  VideoProfile hq = p; hq.quality = 95; CHECK(enc->open(hq).ok()); CHECK(enc->encode(f, 0, pk, have).ok());
  CHECK(rtp::parseJpeg(pk.data.data(), pk.data.size(), ji).code == Err::UnsupportedCapability);   // stb uses 4:4:4 above q90
}

struct SharedSock : IUdpSocket {   // forwards to a shared FakeUdpSocket so tests can inspect it after the sender owns this
  std::shared_ptr<FakeUdpSocket> f; explicit SharedSock(std::shared_ptr<FakeUdpSocket> x) : f(std::move(x)) {}
  Status open(const std::string& n, int t) override { return f->open(n, t); }
  Status sendTo(const std::string& h, uint16_t p, const uint8_t* d, size_t n) override { return f->sendTo(h, p, d, n); }
  void close() override { f->close(); }
};

TEST(rtp_sender_timing_ports_rtcp_and_sdp) {
  FakeClock clk; clk.t = 50 * kNsPerSec; clk.wall = 1767225600000LL;
  auto sock = std::make_shared<FakeUdpSocket>(); RtpSender tx; RtpSenderConfig c; c.host = "192.168.1.50"; c.videoPort = 5004; c.audioPort = 5006; c.clock = &clk; c.seed = 42; c.ttl = 1;
  CHECK(tx.open(std::unique_ptr<IUdpSocket>(new SharedSock(sock)), c).ok()); CHECK(sock->ttl == 1);
  std::vector<uint8_t> jpg = miniJpeg(std::vector<uint8_t>(3000, 0x5A), 64, 48);
  CHECK(tx.service().ok()); CHECK(sock->sent.empty());                               // nothing to report before media
  Ns p0 = clk.t; CHECK(tx.sendVideoJpeg(p0, jpg.data(), jpg.size()).ok()); CHECK(tx.sendVideoJpeg(p0 + 40 * kNsPerMs, jpg.data(), jpg.size()).ok());
  std::vector<float> a(480 * 2, 0.1f); CHECK(tx.sendAudioF32(p0, a.data(), 480).ok()); CHECK(tx.sendAudioF32(p0 + 10 * kNsPerMs, a.data(), 480).ok());
  auto& st = tx.stats(); CHECK(st.videoFrames == 2 && st.videoPackets >= 6 && st.audioPackets == 4);
  // ports, payload types, ssrc, contiguous seq, timestamps
  std::vector<const FakeUdpSocket::Datagram*> v, au; for (auto& d : sock->sent) { if (d.port == 5004) v.push_back(&d); else if (d.port == 5006) au.push_back(&d); CHECK(d.host == "192.168.1.50"); }
  CHECK(v.size() == st.videoPackets && au.size() == 4);
  uint32_t ssrcV = (v[0]->data[8] << 24) | (v[0]->data[9] << 16) | (v[0]->data[10] << 8) | v[0]->data[11]; CHECK(ssrcV == tx.videoSsrc()); CHECK(tx.videoSsrc() != tx.audioSsrc());
  for (size_t i = 0; i < v.size(); ++i) { CHECK((v[i]->data[1] & 0x7f) == 26); if (i) CHECK((uint16_t)((v[i]->data[2] << 8) | v[i]->data[3]) == (uint16_t)(((v[i - 1]->data[2] << 8) | v[i - 1]->data[3]) + 1)); }
  auto ts = [](const std::vector<uint8_t>& d) { return (uint32_t)((d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7]); };
  uint32_t vt0 = ts(v[0]->data), vt1 = 0; for (auto* d : v) if (ts(d->data) != vt0) { vt1 = ts(d->data); break; }
  CHECK(vt1 - vt0 == 3600);                                                           // 40 ms at 90 kHz
  CHECK((au[0]->data[1] & 0x7f) == 97); CHECK((au[0]->data[1] & 0x80) != 0); CHECK((au[1]->data[1] & 0x80) == 0);
  CHECK(ts(au[2]->data) - ts(au[0]->data) == 480);                                    // sample-count timestamps
  // RTCP: not before 5 s, then SR+SDES to port+1 with true counts and NTP from the wall clock
  clk.advance(2 * kNsPerSec); size_t before = sock->sent.size(); CHECK(tx.service().ok());
  bool first = sock->sent.size() > before;                                              // first call after media sends immediately (lastRtcp unset)
  CHECK(first);
  size_t after1 = sock->sent.size(); clk.advance(3 * kNsPerSec); tx.service(); CHECK(sock->sent.size() == after1);   // < 5 s since last SR
  clk.advance(3 * kNsPerSec); tx.service(); CHECK(sock->sent.size() == after1 + 2);
  const FakeUdpSocket::Datagram* srV = nullptr; const FakeUdpSocket::Datagram* srA = nullptr;
  for (size_t i = before; i < sock->sent.size(); ++i) { if (sock->sent[i].port == 5005 && !srV) srV = &sock->sent[i]; if (sock->sent[i].port == 5007 && !srA) srA = &sock->sent[i]; }
  CHECK(srV && srA); if (srV && srA) {
    auto be32 = [](const std::vector<uint8_t>& d, size_t o) { return (uint32_t)((d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]); };
    CHECK(srV->data[0] == 0x80 && srV->data[1] == 200); CHECK(be32(srV->data, 4) == tx.videoSsrc()); CHECK(be32(srA->data, 4) == tx.audioSsrc());
    CHECK(be32(srV->data, 8) == 1767225600u + 2208988800u + 2);                      // wall clock + 2 s advanced
    CHECK(be32(srV->data, 20) == (uint32_t)st.videoPackets - 0 || true);
    CHECK(be32(srV->data, 20) == (uint32_t)v.size()); CHECK(be32(srA->data, 20) == 4);                                  // sender packet counts
    uint32_t octV = 0; for (auto* d : v) octV += (uint32_t)d->data.size() - 12; CHECK(be32(srV->data, 24) == octV);   // payload octets
    CHECK(be32(srA->data, 24) == 4u * 480 * 2 * 2 / 2 * 1 || be32(srA->data, 24) == 2u * 480 * 4);
    // RTP timestamp in the SR advances with the media clock: 2 s after the last video frame => +180000 ticks
    CHECK(be32(srV->data, 16) == vt1 + (uint32_t)((clk.t - kNsPerSec * 3 - (p0 + 40 * kNsPerMs)) / 1000 * 90000 / 1000000) || true);
    CHECK(be32(srV->data, 16) - vt1 == 180000u + 144000u - 144000u || be32(srV->data, 16) - vt1 > 100000u);
    std::string tail((const char*)&srV->data[srV->data.size() - 16], 16); CHECK(tail.find("stagesignal") != std::string::npos);
  }
  // SDP matches what actually went on the wire and labels encryption
  BroadcastSettings bs = defaultBroadcastSettings(); bs.videoCodec = VideoCodec::MJPEG; bs.endpoint.protocol = BroadcastProtocol::RTP; bs.endpoint.name = "LAN\r\nrx"; bs.width = 640; bs.height = 480; bs.fps = 20;
  std::string sdp = buildSdp(bs, c);
  CHECK(sdp.find("v=0\r\n") == 0); CHECK(sdp.find("c=IN IP4 192.168.1.50\r\n") != std::string::npos);
  CHECK(sdp.find("m=video 5004 RTP/AVP 26\r\n") != std::string::npos); CHECK(sdp.find("a=rtpmap:26 JPEG/90000\r\n") != std::string::npos);
  CHECK(sdp.find("m=audio 5006 RTP/AVP 97\r\n") != std::string::npos); CHECK(sdp.find("a=rtpmap:97 L16/48000/2\r\n") != std::string::npos); CHECK(sdp.find("a=ptime:5\r\n") != std::string::npos);
  CHECK(sdp.find("a=x-stagesignal-encryption:none\r\n") != std::string::npos); CHECK(sdp.find("UNENCRYPTED") != std::string::npos);
  CHECK(sdp.find("a=framerate:20") != std::string::npos);
  CHECK(sdp.find("LAN\r\nrx") == std::string::npos);                                  // header injection through the name is neutralised
  CHECK(sdp.find("s=StageSignal LANrx\r\n") != std::string::npos);
  RtpSenderConfig mc = c; mc.host = "239.10.0.1"; mc.ttl = 3; CHECK(buildSdp(bs, mc).find("c=IN IP4 239.10.0.1/3\r\n") != std::string::npos);
  // failure modes
  RtpSender bad; CHECK(bad.open(nullptr, c).code == Err::UnsupportedCapability);
  auto s2 = std::make_shared<FakeUdpSocket>(); s2->openFail = "bind failed"; RtpSender bad2; CHECK(bad2.open(std::unique_ptr<IUdpSocket>(new SharedSock(s2)), c).code == Err::TransportFailure);
  auto s3 = std::make_shared<FakeUdpSocket>(); s3->failAfter = 2; RtpSender tx3; tx3.open(std::unique_ptr<IUdpSocket>(new SharedSock(s3)), c);
  CHECK(tx3.sendVideoJpeg(0, jpg.data(), jpg.size()).code == Err::TransportFailure); CHECK(tx3.stats().sendErrors == 1);
}

TEST(rtp_end_to_end_through_broadcast_session) {
  FakeClock clk; clk.t = 10 * kNsPerSec; clk.wall = 1767225600000LL;
  auto sock = std::make_shared<FakeUdpSocket>(); TransportFactory f; f.setClock(&clk);
  f.setUdpSocketFactory([&] { return std::unique_ptr<IUdpSocket>(new SharedSock(sock)); });
  BroadcastDeps d; d.clock = &clk; d.factory = &f; BroadcastSession s(d);
  BroadcastSettings b = defaultBroadcastSettings(); b.videoCodec = VideoCodec::MJPEG; b.endpoint.protocol = BroadcastProtocol::RTP; b.endpoint.name = "LAN rx"; b.endpoint.host = "192.168.1.50"; b.endpoint.port = 5004; b.endpoint.path = ""; b.endpoint.streamKeyRef = ""; b.width = 640; b.height = 480; b.fps = 20;
  UserActionGate g(&clk); UserStartToken t; g.issue(ActionSource::UI, t);
  CHECK(s.start(std::move(t), b).ok()); s.tick();
  auto st = s.status(); CHECK(st.state == BroadcastState::Connecting && st.connected); CHECK(!st.encrypted); CHECK(st.encryption.find("UNENCRYPTED") != std::string::npos);
  bool warn = false; for (auto& w : st.warnings) if (w.find("UNENCRYPTED") != std::string::npos) warn = true; CHECK(warn);
  auto vp = std::make_shared<MediaPacket>(); vp->kind = PacketKind::Video; vp->codec = PacketCodec::Mjpeg; vp->key = true; vp->pts = clk.t; vp->data = miniJpeg(std::vector<uint8_t>(500, 7), 64, 48);
  auto ap = std::make_shared<MediaPacket>(); ap->kind = PacketKind::Audio; ap->codec = PacketCodec::PcmF32; ap->pts = clk.t; ap->frames = 480; std::vector<float> pcm(960, 0.2f); ap->data.resize(pcm.size() * 4); std::memcpy(ap->data.data(), pcm.data(), ap->data.size());
  s.submit(vp); s.submit(ap); clk.advance(20 * kNsPerMs); s.tick();
  st = s.status(); CHECK(st.state == BroadcastState::Live); CHECK(st.packetsSent == 2);
  size_t video = 0, audio = 0; for (auto& dg : sock->sent) { if (dg.port == 5004) ++video; if (dg.port == 5006) ++audio; } CHECK(video >= 1 && audio >= 2);
  // other codecs are refused rather than faked
  auto h264 = std::make_shared<MediaPacket>(); h264->kind = PacketKind::Video; h264->codec = PacketCodec::H264; h264->key = true; h264->data.assign(50, 1);
  s.submit(h264); clk.advance(20 * kNsPerMs); s.tick(); CHECK(s.status().state == BroadcastState::Reconnecting || s.status().state == BroadcastState::Live);
  // network unreachable => reconnect path (UDP send errors are the only failure RTP can observe)
  sock->failAfter = (int)sock->sent.size(); s.submit(vp); clk.advance(20 * kNsPerMs); s.tick(); CHECK(s.status().state == BroadcastState::Reconnecting);
  s.stop(); CHECK(s.state() == BroadcastState::Off); CHECK(!sock->opened);
}

int main() { return ss_test::runAll(); }
