// MKV H.264 storage (V_MPEG4/ISO/AVC + avcC CodecPrivate). The encoder is a FAKE producing synthetic Annex B NAL units (not real H.264
// pictures); the independent re-parser (tests/rec_check.h) and an independent AVCC walker verify what the writer stored.
#include "test.h"
#include "rec_check.h"
#include "recording/session.h"
#include "recording/h264.h"
using namespace ss; using namespace reccheck;

static const std::vector<uint8_t> kSps{0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x05, 0x07, 0xE8}, kPps{0x68, 0xCE, 0x3C, 0x80};
struct FakeH264 : IVideoEncoder {
  int n = 0; std::vector<std::vector<uint8_t>> sentAnnexB; bool noPriv = false;
  Status open(const VideoProfile& p) override { return p.codec == VideoCodec::H264 ? Status::Ok() : Status::Fail(Err::InvalidArgument, "h264 only"); }
  Status encode(const Frame& f, Ns pts, EncodedPacket& out, bool& have) override {
    bool key = n % 5 == 0; std::vector<uint8_t> a{0, 0, 0, 1, 0x09, 0xF0};
    if (key) { a.insert(a.end(), {0, 0, 0, 1}); a.insert(a.end(), kSps.begin(), kSps.end()); a.insert(a.end(), {0, 0, 1}); a.insert(a.end(), kPps.begin(), kPps.end()); }
    a.insert(a.end(), {0, 0, 0, 1, (uint8_t)(key ? 0x65 : 0x41)}); for (int i = 0; i < 40 + n; ++i) a.push_back((uint8_t)(0x10 + n + i * 3));
    a.insert(a.end(), {0, 0, 1, 0x06, 0x05, 0x01, 0x80}); ++n;   // trailing SEI as a second NAL (kept)
    sentAnnexB.push_back(a); out.data = a; out.pts = pts; out.key = key; have = true; return Status::Ok();
  }
  void close() override {}
  Ns delayNs() const override { return 0; }
  std::string name() const override { return "fake-h264"; }
  VideoCodec codec() const override { return VideoCodec::H264; }
  std::vector<uint8_t> codecPrivate() const override { return noPriv ? std::vector<uint8_t>() : h264::buildAvcC(kSps, kPps); }
};
static std::shared_ptr<FakeH264> g_enc;
static AudioBlockPtr blk(uint64_t first, size_t frames, Ns baseCap) {
  auto b = std::make_shared<AudioBlock>(); b->rate = 48000; b->channels = 2; b->firstSample = first; b->flags = AF_TimestampValid; b->captureNs = baseCap + rec::samplesToNs(first, 48000); b->samples.assign(frames * 2, 0.25f); return b;
}
struct Rig {
  MemFileSystem fs; FakeDiskInfo disk; FakeClock clk; EncoderRegistry reg; std::unique_ptr<RecordingSession> s; Ns base = 1000 * kNsPerSec;
  struct Proxy : IVideoEncoder { std::shared_ptr<FakeH264> t; Status open(const VideoProfile& p) override { return t->open(p); } Status encode(const Frame& f, Ns p, EncodedPacket& o, bool& h) override { return t->encode(f, p, o, h); } void close() override {} Ns delayNs() const override { return 0; } std::string name() const override { return t->name(); } VideoCodec codec() const override { return VideoCodec::H264; } std::vector<uint8_t> codecPrivate() const override { return t->codecPrivate(); } };
  explicit Rig(bool noPriv = false) {
    clk.wall = 1767225600000LL; clk.t = base; disk.free = 50ull << 30; g_enc = std::make_shared<FakeH264>(); g_enc->noPriv = noPriv;
    reg.add(VideoCodec::H264, "fake-h264", [] { auto* p = new Proxy(); p->t = g_enc; return std::unique_ptr<IVideoEncoder>(p); }, false, "test fake");
    SessionDeps d; d.fs = &fs; d.disk = &disk; d.clock = &clk; d.encoders = &reg; d.manualPump = true; s.reset(new RecordingSession(d));
  }
  RecordingRequest req(RecContainer c) {
    RecordingRequest r; r.profile.container = c; r.dir = "rec"; r.showName = "h264"; r.sessionId = "s1"; r.sources.audioPresent = true; r.sources.audioRate = 48000; r.sources.audioChannels = 2; r.profile.segmentNs = 3600ll * kNsPerSec;
    r.profile.video.codec = VideoCodec::H264; r.profile.video.width = 64; r.profile.video.height = 48; r.profile.video.fpsNum = 25; r.profile.video.bitrateKbps = 500; r.sources.videoPresent = true; r.sources.videoW = 64; r.sources.videoH = 48; return r;
  }
};
// independent AVCC walker: returns NAL list or empty on malformed
static std::vector<std::vector<uint8_t>> walkAvcc(const std::vector<uint8_t>& d) {
  std::vector<std::vector<uint8_t>> v; size_t p = 0;
  while (p < d.size()) { if (p + 4 > d.size()) return {}; size_t l = (size_t)d[p] << 24 | d[p + 1] << 16 | d[p + 2] << 8 | d[p + 3]; p += 4; if (l == 0 || p + l > d.size()) return {}; v.emplace_back(d.begin() + p, d.begin() + p + l); p += l; }
  return v;
}

TEST(mkv_stores_h264_with_avcc_private_and_length_prefixed_blocks) {
  Rig r; CHECK(r.s->start(r.req(RecContainer::Mkv)).ok());
  CHECK(r.s->status().warnings.size() >= 1);                                  // unqualified encoder is disclosed
  uint64_t k = 0;
  for (int i = 0; i < 50; ++i) {                                              // 0.5 s of audio, 10 frames of video
    r.clk.t = r.base + rec::samplesToNs(k * 480, 48000); r.s->pushAudio(blk(k * 480, 480, r.base)); ++k;
    if (i % 5 == 0) { auto f = std::make_shared<Frame>(); f->resize(64, 48); f->pts = r.base + i * 10 * kNsPerMs; r.s->pushVideo(f); }
    r.s->pump();
  }
  CHECK(r.s->stop().ok()); RecordingStatus st = r.s->status(); CHECK(st.segments.size() == 1);
  rec::Bytes b; CHECK(r.fs.readAll(st.segments[0].path, b)); Mkv m = parseMkv(b); CHECK(m.ok); if (!m.ok) std::printf("  %s\n", m.err.c_str());
  CHECK(m.tracks.size() == 2 && m.tracks[0].codec == "V_MPEG4/ISO/AVC" && m.tracks[0].w == 64 && m.tracks[0].h == 48);
  CHECK(m.tracks[0].priv == h264::buildAvcC(kSps, kPps));
  { std::vector<uint8_t> s2, p2; int ls = 0; CHECK(h264::parseAvcC(m.tracks[0].priv.data(), m.tracks[0].priv.size(), s2, p2, &ls) && s2 == kSps && p2 == kPps && ls == 4); }
  CHECK(m.seekVerified && m.cues > 0);
  int vb = 0; for (auto& bl : m.blocks) if (bl.track == 1) {
    auto nals = walkAvcc(bl.data); CHECK(!nals.empty()); bool idr = false, sei = false;
    for (auto& n : nals) { int t = n[0] & 31; CHECK(t != 7 && t != 8 && t != 9); if (t == 5) idr = true; if (t == 6) sei = true; }   // parameter sets / AUD stripped
    CHECK(sei); CHECK(idr == bl.key);
    // exact payload: the slice bytes equal what the fake encoder produced (after the start code)
    const auto& src = g_enc->sentAnnexB[vb]; auto srcNals = h264::splitAnnexB(src.data(), src.size()); size_t j = 0;
    for (auto& sn : srcNals) { if (sn.type() == 7 || sn.type() == 8 || sn.type() == 9) continue; CHECK(j < nals.size() && nals[j] == std::vector<uint8_t>(sn.p, sn.p + sn.n)); ++j; }
    ++vb; }
  CHECK(vb == 10); CHECK(m.audioBlockCount > 0);
}

TEST(h264_storage_refusals_are_honest) {
  { Rig r; Status s = r.s->start(r.req(RecContainer::Avi)); CHECK(s.code == Err::UnsupportedCapability && s.detail.find("MKV") != std::string::npos); CHECK(r.s->state() == RecState::Failed); }
  { Rig r(true); Status s = r.s->start(r.req(RecContainer::Mkv)); CHECK(!s.ok() && s.detail.find("avcC") != std::string::npos); }
  MemFileSystem fs; MediaWriterConfig c; c.dir = "d"; c.base = "B"; c.codec = VideoCodec::H264; c.width = 64; c.height = 48;
  { auto w = MediaWriter::create(MediaContainer::Avi, fs); CHECK(w->open(c).code == Err::UnsupportedCapability); }
  { auto w = MediaWriter::create(MediaContainer::Mkv, fs); CHECK(w->open(c).code == Err::InvalidArgument); }   // no avcC
}
int main() { return ss_test::runAll(); }
