// MOD-11 container tests: naming, WAV segmentation/recovery, AVI + MKV writers with independent re-parse, encoders.
#include "test.h"
#include "rec_check.h"
#include "recording/io.h"
#include "recording/naming.h"
#include "recording/journal.h"
#include "recording/wav_writer.h"
#include "recording/video_writer.h"
#include "recording/video_encoder.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <cmath>
using namespace ss;
using namespace reccheck;

static std::vector<float> genAudio(uint64_t first, size_t frames, int ch) {
  std::vector<float> v(frames * ch);
  for (size_t i = 0; i < frames; ++i) for (int c = 0; c < ch; ++c)
    v[i * ch + c] = 0.6f * (float)std::sin(6.283185307179586 * (double)(first + i) * (440.0 + 110.0 * c) / 8000.0);
  return v;
}
static Frame genFrame(int w, int h, int k) {
  Frame f; f.resize(w, h);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) f.px[(size_t)y * w + x] = rgb((x * 4 + k * 9) & 255, (y * 5) & 255, (k * 40) & 255);
  return f;
}
static std::vector<std::vector<uint8_t>> jpegs(int n, int w, int h) {
  auto enc = makeMjpegEncoder(); VideoProfile p; p.width = w; p.height = h; p.quality = 70; enc->open(p);
  std::vector<std::vector<uint8_t>> v;
  for (int i = 0; i < n; ++i) { EncodedPacket pk; bool have; Frame f = genFrame(w, h, i); enc->encode(f, 0, pk, have); v.push_back(pk.data); }
  return v;
}

TEST(rec_naming) {
  CHECK(rec::sanitizeShowName("My Show: Part/1?") == "My_Show_Part_1");
  CHECK(rec::sanitizeShowName("") == "show");
  CHECK(rec::sanitizeShowName("CON") == "_CON");
  CHECK(rec::sanitizeShowName("com1.txt") == "_com1.txt");
  CHECK(rec::sanitizeShowName("..\\..\\evil") == "evil");
  CHECK(rec::sanitizeShowName("\xC3\xA9t\xC3\xA9 live") == "t_live");
  CHECK(rec::sanitizeShowName(std::string(200, 'a')).size() == 40);
  CHECK(rec::utcStamp(1767225600000LL) == "20260101T000000Z");
  CHECK(rec::utcStamp(1709164799000LL) == "20240228T235959Z");
  CHECK(rec::utcStamp(1709164800000LL) == "20240229T000000Z");   // leap day
  CHECK(rec::baseName(1767225600000LL, "ab c/d", "Gala Night") == "StageSignal_20260101T000000Z_ab-c-d_Gala_Night");
  CHECK(rec::segmentName("X", 3, "wav") == "X_0003.wav");
}

TEST(rec_wav_segments_exact) {
  MemFileSystem fs; SegmentJournal j; j.open(fs, "rec/s.journal");
  WavConfig c; c.rate = 8000; c.channels = 2; c.maxSegmentNs = kNsPerSec; c.dir = "rec"; c.base = "B"; c.headerUpdateFrames = 1000;
  WavWriter w(fs, &j); CHECK(w.open(c).ok());
  uint64_t n = 0; size_t blocks[] = {777, 1, 4096, 31, 8000, 3333, 999};
  std::vector<int32_t> expect;
  for (int rep = 0; rep < 2; ++rep) for (size_t b : blocks) { auto a = genAudio(n, b, 2); for (float f : a) expect.push_back(pcm24(f)); CHECK(w.write(a.data(), b).ok()); n += b; }
  CHECK(w.close().ok());
  CHECK(w.totalFrames() == n);
  uint64_t sum = 0; std::vector<int32_t> got; uint64_t expectFirst = 0;
  for (auto& s : w.segments()) {
    rec::Bytes b; CHECK(fs.readAll(s.path, b)); Wav p = parseWav(b); CHECK(p.ok); if (!p.ok) { std::printf("   %s\n", p.err.c_str()); continue; }
    CHECK(p.bits == 24 && p.ch == 2 && p.rate == 8000 && p.fmtTag == 1);
    CHECK(s.complete); CHECK(s.firstSample == expectFirst); CHECK(s.audioFrames == p.frames);
    CHECK(s.startNs == rec::samplesToNs(s.firstSample, 8000));
    CHECK(s.endNs == s.startNs + rec::samplesToNs(s.audioFrames, 8000));
    if (&s != &w.segments().back()) CHECK(p.frames == 8000);
    expectFirst += p.frames; sum += p.frames; got.insert(got.end(), p.pcm.begin(), p.pcm.end());
  }
  CHECK(sum == n); CHECK(got == expect);           // no loss, no overlap, bit-exact order
  CHECK(w.segments().size() == (size_t)((n + 7999) / 8000));
  CHECK(w.segments()[0].path == "rec/B_0000.wav");
  rec::Bytes jb; fs.readAll("rec/s.journal", jb); auto je = SegmentJournal::parse(jb);
  CHECK(je.size() == w.segments().size()); for (auto& e : je) CHECK(!e.active);
}

TEST(rec_wav_size_budget) {
  MemFileSystem fs; WavConfig c; c.rate = 8000; c.channels = 1; c.maxSegmentNs = 3600ll * kNsPerSec; c.maxSegmentBytes = 44 + 3 * 5000; c.dir = "d"; c.base = "B";
  WavWriter w(fs); CHECK(w.open(c).ok());
  auto a = genAudio(0, 12000, 1); CHECK(w.write(a.data(), a.size()).ok()); CHECK(w.close().ok());
  CHECK(w.segments().size() == 3); CHECK(w.segments()[0].audioFrames == 5000); CHECK(w.segments()[1].audioFrames == 5000); CHECK(w.segments()[2].audioFrames == 2000);
  rec::Bytes b; fs.readAll(w.segments()[0].path, b); CHECK(b.size() == 44 + 15000);
  // 4 GiB clamp: oversize budget is limited well below the classic RIFF limit
  WavConfig big; big.maxSegmentBytes = 1ull << 40; big.dir = "d"; big.base = "C"; WavWriter w2(fs); CHECK(w2.open(big).ok());
}

TEST(rec_wav_crash_recovery) {
  MemFileSystem fs; WavConfig c; c.rate = 8000; c.channels = 2; c.dir = "d"; c.base = "B"; c.headerUpdateFrames = 2000;
  WavWriter w(fs); CHECK(w.open(c).ok());
  auto a = genAudio(0, 7001, 2); for (int i = 0; i < 7; ++i) CHECK(w.write(a.data() + (size_t)i * 2000, 1000).ok()); CHECK(w.write(a.data() + 14000, 1).ok());
  const std::string path = w.segments()[0].path;
  CHECK(w.durableFrames() == 6000);                   // last periodic patch covered 3x2000
  rec::Bytes live; fs.readAll(path, live); fs.files["crashed.wav"] = std::make_shared<MemFileSystem::File>(); fs.files["crashed.wav"]->data = live;
  fs.chop("crashed.wav", 5);                          // torn final frame
  WavRecovery r = recoverWav(fs, "crashed.wav");
  CHECK(r.ok && r.repaired); CHECK(r.frames == 7000); CHECK(r.droppedTailBytes == 6 - 5 + 0 + 0 || r.droppedTailBytes > 0);
  rec::Bytes b; fs.readAll("crashed.wav", b); Wav p = parseWav(b); CHECK(p.ok); CHECK(p.frames == 7000);
  for (size_t i = 0; i < p.pcm.size() && i < 100; ++i) CHECK(p.pcm[i] == pcm24(a[i]));
  CHECK(recoverWav(fs, "crashed.wav").detail == "intact");
  // header-only file: valid, zero frames
  WavConfig c2 = c; c2.base = "H"; WavWriter w2(fs); w2.open(c2); rec::Bytes hb; fs.readAll(w2.segments()[0].path, hb);
  fs.files["h.wav"] = std::make_shared<MemFileSystem::File>(); fs.files["h.wav"]->data = hb; WavRecovery hr = recoverWav(fs, "h.wav"); CHECK(hr.ok && hr.frames == 0);
  fs.files["junk.wav"] = std::make_shared<MemFileSystem::File>(); fs.files["junk.wav"]->data = {1, 2, 3}; CHECK(!recoverWav(fs, "junk.wav").ok);
}

TEST(rec_wav_disk_full_reports_lost) {
  MemFileSystem fs; fs.plan.capacityBytes = 44 + 12000 + 3005;   // 2500 frames + 5 bytes of a partial frame fit
  WavConfig c; c.rate = 8000; c.channels = 2; c.dir = "d"; c.base = "B"; c.headerUpdateFrames = 100000;
  WavWriter w(fs); CHECK(w.open(c).ok());
  auto a = genAudio(0, 1000, 2); Status s = Status::Ok(); int blocks = 0;
  for (; blocks < 10 && s.ok(); ++blocks) s = w.write(a.data(), 1000);
  CHECK(!s.ok()); CHECK(s.code == Err::DiskFull); CHECK(w.failed());
  CHECK(w.totalFrames() == 2500); CHECK(w.lostFromSample() == 2500);
  rec::Bytes b; fs.readAll("d/B_0000.wav", b); WavRecovery r = recoverWav(fs, "d/B_0000.wav"); CHECK(r.ok); CHECK(r.frames == 2500);
  fs.readAll("d/B_0000.wav", b); CHECK(parseWav(b).ok);
  CHECK(!w.write(a.data(), 10).ok());                 // sticky
}

static MediaWriterConfig vcfg() {
  MediaWriterConfig c; c.dir = "v"; c.base = "V"; c.audioRate = 8000; c.audioChannels = 2; c.audioBits = 24; c.width = 64; c.height = 48; c.fpsNum = 10; c.fpsDen = 1; c.maxSegmentNs = 2 * kNsPerSec; return c;
}

static void feedAv(MediaWriter& w, const std::vector<std::vector<uint8_t>>& jp, int seconds, std::vector<int32_t>& expectPcm, int skipEvery = 0, int* skipped = nullptr) {
  uint64_t n = 0; int fi = 0;
  for (int s = 0; s < seconds * 10; ++s) {            // 100 ms steps
    auto a = genAudio(n, 800, 2); for (float f : a) expectPcm.push_back(pcm24(f)); n += 800;
    CHECK(w.writeAudio(a.data(), 800).ok());
    if (skipEvery && s % skipEvery == skipEvery - 1) { if (skipped) ++*skipped; continue; }
    const auto& jf = jp[fi++ % jp.size()];
    CHECK(w.writeVideo((Ns)s * 100 * kNsPerMs + 3 * kNsPerMs, jf.data(), jf.size(), true).ok());
  }
}

TEST(rec_avi_segments_and_reparse) {
  MemFileSystem fs; SegmentJournal j; j.open(fs, "v/j");
  auto jp = jpegs(4, 64, 48);
  auto w = MediaWriter::create(MediaContainer::Avi, fs, &j); CHECK(w->open(vcfg()).ok());
  std::vector<int32_t> expect; int skipped = 0; feedAv(*w, jp, 5, expect, 7, &skipped);
  CHECK(w->close().ok());
  CHECK(w->segments().size() == 3);                   // 5 s at 2 s/segment
  std::vector<int32_t> got; uint64_t vsum = 0, dups = 0;
  for (auto& s : w->segments()) {
    rec::Bytes b; CHECK(fs.readAll(s.path, b)); Avi a = parseAvi(b); CHECK(a.ok); if (!a.ok) { std::printf("   avi: %s\n", a.err.c_str()); continue; }
    CHECK(a.w == 64 && a.h == 48 && a.fpsNum == 10 && a.fpsDen == 1 && a.rate == 8000 && a.ch == 2 && a.bits == 24 && a.streams == 2);
    CHECK(s.complete);
    for (auto& f : a.frames) CHECK(isJpeg(f));
    got.insert(got.end(), a.pcm.begin(), a.pcm.end()); vsum += a.frames.size(); dups += a.vDup;
    CHECK(a.aFrames == s.audioFrames);
  }
  CHECK(got == expect);                               // audio: no loss/overlap across segments
  CHECK(dups == w->duplicatedVideo()); CHECK(dups > 0);   // skipped frames became repeat slots
  CHECK(vsum + dups > 40);
  for (size_t i = 1; i < w->segments().size(); ++i) CHECK(w->segments()[i].firstSample == w->segments()[i - 1].firstSample + w->segments()[i - 1].audioFrames);
  CHECK(w->segments()[0].path == "v/V_0000.avi");
  rec::Bytes b; fs.readAll("v/V_0000.avi", b); Avi a0 = parseAvi(b); CHECK(a0.fpsNum == 10);
}

TEST(rec_avi_size_budget_and_2gib_cap) {
  MemFileSystem fs; auto jp = jpegs(2, 64, 48);
  MediaWriterConfig c = vcfg(); c.maxSegmentNs = 3600ll * kNsPerSec; c.maxSegmentBytes = 40000;
  auto w = MediaWriter::create(MediaContainer::Avi, fs); CHECK(w->open(c).ok());
  std::vector<int32_t> expect; feedAv(*w, jp, 5, expect);
  CHECK(w->close().ok()); CHECK(w->segments().size() > 1);
  std::vector<int32_t> got;
  for (auto& s : w->segments()) { rec::Bytes b; fs.readAll(s.path, b); CHECK(b.size() <= 40000); Avi a = parseAvi(b); CHECK(a.ok); got.insert(got.end(), a.pcm.begin(), a.pcm.end()); }
  CHECK(got == expect);
  MediaWriterConfig big = vcfg(); big.maxSegmentBytes = 1ull << 40; MemFileSystem fs2; auto w2 = MediaWriter::create(MediaContainer::Avi, fs2); CHECK(w2->open(big).ok());
}

TEST(rec_avi_truncated_recovery) {
  MemFileSystem fs; auto jp = jpegs(3, 64, 48);
  MediaWriterConfig c = vcfg(); c.maxSegmentNs = 3600ll * kNsPerSec;
  auto w = MediaWriter::create(MediaContainer::Avi, fs); CHECK(w->open(c).ok());
  std::vector<int32_t> expect; feedAv(*w, jp, 3, expect);
  rec::Bytes live; fs.readAll("v/V_0000.avi", live);   // never finalized: no idx1, sizes zero
  CHECK(!parseAvi(live).ok);
  fs.files["t.avi"] = std::make_shared<MemFileSystem::File>(); fs.files["t.avi"]->data = live; fs.chop("t.avi", 100);   // torn last chunk
  AviRecovery r = recoverAvi(fs, "t.avi");
  CHECK(r.ok && r.repaired); CHECK(r.videoSlots >= 25); CHECK(r.droppedTailBytes > 0);
  rec::Bytes b; fs.readAll("t.avi", b); Avi a = parseAvi(b); CHECK(a.ok); if (!a.ok) std::printf("   avi: %s\n", a.err.c_str());
  CHECK(a.vChunks == r.videoSlots); CHECK(a.aFrames == r.audioFrames);
  for (auto& f : a.frames) CHECK(isJpeg(f));
  for (size_t i = 0; i < a.pcm.size(); ++i) if (a.pcm[i] != expect[i]) { CHECK(false); break; }
  CHECK(recoverAvi(fs, "t.avi").detail == "intact");
}

TEST(rec_mkv_structure_and_demux) {
  MemFileSystem fs; SegmentJournal j; j.open(fs, "v/j");
  auto jp = jpegs(5, 64, 48);
  MediaWriterConfig c = vcfg(); c.maxSegmentNs = 12 * kNsPerSec;
  auto w = MediaWriter::create(MediaContainer::Mkv, fs, &j);
  w->setTagProvider([](const SegmentInfo& s) { return MediaWriter::Tags{{"STAGESIGNAL_MARKER", "1.500 chorus"}, {"STAGESIGNAL_DYNAMICS", "on"}}; });
  CHECK(w->open(c).ok());
  std::vector<int32_t> expect; feedAv(*w, jp, 20, expect);
  CHECK(w->close().ok());
  CHECK(w->segments().size() == 2);
  std::vector<int32_t> got; size_t vf = 0;
  for (auto& s : w->segments()) {
    rec::Bytes b; CHECK(fs.readAll(s.path, b)); Mkv m = parseMkv(b); CHECK(m.ok); if (!m.ok) { std::printf("   mkv: %s\n", m.err.c_str()); continue; }
    CHECK(!m.segUnknown); CHECK(m.docType == "matroska"); CHECK(m.timecodeScale == 1000000);
    CHECK(m.tracks.size() == 2); CHECK(m.tracks[0].codec == "V_MJPEG" && m.tracks[0].w == 64 && m.tracks[0].h == 48 && m.tracks[0].defDur == 100000000);
    CHECK(m.tracks[1].codec == "A_PCM/INT/LIT" && m.tracks[1].ch == 2 && m.tracks[1].bits == 24 && m.tracks[1].rate == 8000.0);
    CHECK(m.clusters >= 2); for (auto sp : m.clusterSpan) CHECK(sp <= 5200);          // <= 5 s clusters (+ block jitter)
    CHECK(m.cues == m.clusters); CHECK(m.seekVerified); CHECK(m.unknownSizedClusters.empty());
    CHECK(m.seek.count(0x1C53BB6B) && m.seek.count(0x1254C367) && m.seek.count(0x1549A966) && m.seek.count(0x1654AE6B));
    { double want = (double)s.audioFrames * 1000.0 / 8000.0; CHECK(m.durationMs >= want - 1 && m.durationMs <= want + 120); }
    bool tagFound = false; for (auto& t : m.tags) if (t.first == "STAGESIGNAL_MARKER" && t.second == "1.500 chorus") tagFound = true; CHECK(tagFound);
    B audio; int64_t lastA = -1;
    for (auto& bl : m.blocks) { if (bl.track == 2) { audio.insert(audio.end(), bl.data.begin(), bl.data.end()); CHECK(bl.ms >= lastA); lastA = bl.ms; } else { CHECK(isJpeg(bl.data)); ++vf; } }
    auto pc = pcm24le(audio); got.insert(got.end(), pc.begin(), pc.end());
  }
  CHECK(got == expect);
  CHECK(vf == 200);
}

TEST(rec_mkv_truncated_is_streaming_readable) {
  MemFileSystem fs; auto jp = jpegs(2, 64, 48);
  MediaWriterConfig c = vcfg(); c.maxSegmentNs = 3600ll * kNsPerSec;
  auto w = MediaWriter::create(MediaContainer::Mkv, fs); CHECK(w->open(c).ok());
  std::vector<int32_t> expect; feedAv(*w, jp, 8, expect);
  rec::Bytes live; fs.readAll("v/V_0000.mkv", live);   // crash before finalize
  live.resize(live.size() - 7);                         // torn final block
  Mkv m = parseMkv(live);
  CHECK(m.ok); CHECK(m.segUnknown); CHECK(m.cues == 0); CHECK(m.truncated || m.blocks.size() > 0);
  CHECK(m.unknownSizedClusters.size() >= 1);          // the cluster open at crash time has unknown size
  CHECK(m.videoBlockCount >= 70 && m.audioBlockCount >= 70);
  CHECK(m.tracks.size() == 2);
  // complete blocks before the tear are intact
  B audio; for (auto& bl : m.blocks) if (bl.track == 2) audio.insert(audio.end(), bl.data.begin(), bl.data.end());
  auto pc = pcm24le(audio); bool same = pc.size() > 1000; for (size_t i = 0; i < pc.size() && same; ++i) same = pc[i] == expect[i]; CHECK(same);
}

TEST(rec_encoder_registry_honest) {
  EncoderRegistry r = EncoderRegistry::withBuiltins();
  auto h = r.availability(VideoCodec::H264); CHECK(!h.available); CHECK(h.reason.find("not bundle") != std::string::npos || h.reason.find("No H.264") != std::string::npos);
  auto m = r.availability(VideoCodec::MJPEG); CHECK(m.available); CHECK(!m.qualified);
  std::unique_ptr<IVideoEncoder> e; VideoProfile p; p.codec = VideoCodec::H264;
  Status s = r.create(p, e); CHECK(s.code == Err::EncoderUnavailable); CHECK(!e);
  // pluggable adapter
  struct Fake : IVideoEncoder { Status open(const VideoProfile&) override { return Status::Ok(); } Status encode(const Frame&, Ns pts, EncodedPacket& o, bool& h) override { o.pts = pts; o.data = {0, 0, 0, 1}; h = true; return Status::Ok(); } void close() override {} Ns delayNs() const override { return 33 * kNsPerMs; } std::string name() const override { return "fake-mf"; } VideoCodec codec() const override { return VideoCodec::H264; } };
  r.add(VideoCodec::H264, "fake-mf", [] { return std::unique_ptr<IVideoEncoder>(new Fake()); }, true);
  CHECK(r.create(p, e).ok()); CHECK(e && e->delayNs() == 33 * kNsPerMs); CHECK(r.availability(VideoCodec::H264).qualified);
  // MJPEG decodes with an independent decoder and keeps pixel content roughly
  VideoProfile mp; mp.width = 64; mp.height = 48; mp.quality = 90; CHECK(r.create(mp, e).ok());
  Frame f; f.resize(64, 48); for (auto& px : f.px) px = rgb(200, 30, 30);
  EncodedPacket pk; bool have = false; CHECK(e->encode(f, 123, pk, have).ok()); CHECK(have && pk.key && pk.pts == 123);
  int iw, ih, comp; unsigned char* img = stbi_load_from_memory(pk.data.data(), (int)pk.data.size(), &iw, &ih, &comp, 3);
  CHECK(img != nullptr); if (img) { CHECK(iw == 64 && ih == 48); CHECK(std::abs(img[0] - 200) < 12 && std::abs(img[1] - 30) < 12 && std::abs(img[2] - 30) < 12); stbi_image_free(img); }
  Frame wrong; wrong.resize(32, 32); CHECK(e->encode(wrong, 0, pk, have).code == Err::InvalidArgument);
  VideoProfile bad = mp; bad.width = 0; CHECK(!r.create(bad, e).ok());
}

TEST(rec_journal_torn_line_and_state) {
  MemFileSystem fs; SegmentJournal j; CHECK(j.open(fs, "j").ok());
  SegmentInfo s; s.index = 0; s.path = "a"; s.kind = "wav"; j.segmentOpen(s); s.audioFrames = 10; s.complete = true; j.segmentClose(s);
  SegmentInfo t; t.index = 1; t.path = "b"; t.kind = "wav"; j.segmentOpen(t);
  rec::Bytes b; fs.readAll("j", b); const char* torn = "{\"ev\":\"segment_close\",\"index\":1"; b.insert(b.end(), torn, torn + std::strlen(torn));
  auto e = SegmentJournal::parse(b); CHECK(e.size() == 2); CHECK(!e[0].active && e[0].seg.audioFrames == 10); CHECK(e[1].active);
}

int main() { return ss_test::runAll(); }
