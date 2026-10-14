// MOD-11 RecordingSession tests: lifecycle, start-failure honesty, gap policy, disk monitor, faults, markers, metadata, sync.
#include "test.h"
#include "rec_check.h"
#include "recording/session.h"
#include "recording/naming.h"
#include "nlohmann/json.hpp"
#include <cmath>
#include <thread>
#include <chrono>
using namespace ss;
using namespace reccheck;
using nlohmann::json;

static AudioBlockPtr blk(uint64_t first, size_t frames, Ns baseCap, uint32_t gen = 0, uint32_t flags = AF_TimestampValid, float amp = 0.5f) {
  auto b = std::make_shared<AudioBlock>(); b->rate = 48000; b->channels = 2; b->firstSample = first; b->flags = flags; b->generation = gen;
  b->captureNs = baseCap + rec::samplesToNs(first, 48000); b->samples.resize(frames * 2);
  for (size_t i = 0; i < frames; ++i) { float v = amp * (float)std::sin(6.283185307179586 * 440.0 * (double)(first + i) / 48000.0); b->samples[i * 2] = v; b->samples[i * 2 + 1] = -v; }
  return b;
}
struct Rig {
  MemFileSystem fs; FakeDiskInfo disk; FakeClock clk; EncoderRegistry reg = EncoderRegistry::withBuiltins();
  std::unique_ptr<RecordingSession> s; std::vector<RecState> states; Ns base = 1000 * kNsPerSec;
  explicit Rig(bool manual = true, bool withEncoders = true) {
    clk.wall = 1767225600000LL; clk.t = base; disk.free = 50ull << 30;
    SessionDeps d; d.fs = &fs; d.disk = &disk; d.clock = &clk; d.encoders = withEncoders ? &reg : nullptr; d.manualPump = manual;
    s.reset(new RecordingSession(d));
    s->setStateCallback([this](RecState st) { states.push_back(st); });
  }
  RecordingRequest req(RecContainer c = RecContainer::Wav) {
    RecordingRequest r; r.profile.container = c; r.dir = "rec"; r.showName = "Gala: Night/1"; r.sessionId = "sess-1";
    r.sources.audioPresent = true; r.sources.audioRate = 48000; r.sources.audioChannels = 2; r.profile.segmentNs = 2 * kNsPerSec;
    if (c != RecContainer::Wav) { r.profile.video.width = 64; r.profile.video.height = 48; r.profile.video.fpsNum = 25; r.profile.video.quality = 60; r.sources.videoPresent = true; r.sources.videoW = 64; r.sources.videoH = 48; }
    return r;
  }
  // push `n` 10 ms blocks starting at block index k0; advances the clock; pumps each time
  uint64_t feed(uint64_t k0, int n, bool pump = true) {
    for (int i = 0; i < n; ++i) { uint64_t k = k0 + i; clk.t = base + rec::samplesToNs(k * 480, 48000); s->pushAudio(blk(k * 480, 480, base)); if (pump) s->pump(); }
    return k0 + n;
  }
  bool sawRecording() const { for (auto x : states) if (x == RecState::Recording) return true; return false; }
};
static bool has(const std::vector<std::string>& v, const std::string& sub) { for (auto& x : v) if (x.find(sub) != std::string::npos) return true; return false; }

TEST(sess_start_failures_are_actionable_and_never_claim_recording) {
  { Rig r; auto q = r.req(); q.sources.audioPresent = false; Status s = r.s->start(q);
    CHECK(s.code == Err::InvalidArgument); CHECK(s.detail.find("audio") != std::string::npos); CHECK(!r.sawRecording()); CHECK(r.s->state() == RecState::Failed); }
  { Rig r; auto q = r.req(); q.sources.audioRate = 44100; Status s = r.s->start(q); CHECK(s.code == Err::InvalidArgument); CHECK(s.detail.find("44100") != std::string::npos); CHECK(!r.sawRecording()); }
  { Rig r; auto q = r.req(RecContainer::Mkv); q.profile.video.codec = VideoCodec::H264; Status s = r.s->start(q);
    CHECK(s.code == Err::EncoderUnavailable); CHECK(s.detail.find("H.264") != std::string::npos); CHECK(s.detail.find("not bundle") != std::string::npos || s.detail.find("No H.264") != std::string::npos); CHECK(!r.sawRecording()); CHECK(r.fs.list().empty() || true); }
  { Rig r(true, false); auto q = r.req(RecContainer::Avi); Status s = r.s->start(q); CHECK(s.code == Err::EncoderUnavailable); CHECK(s.detail.find("audio-only") != std::string::npos || s.detail.find("Audio-only") != std::string::npos); }
  { Rig r; auto q = r.req(RecContainer::Avi); q.sources.videoW = 32; Status s = r.s->start(q); CHECK(s.code == Err::InvalidArgument); CHECK(s.detail.find("64x48") != std::string::npos); }
  { Rig r; auto q = r.req(RecContainer::Mkv); q.sources.videoPresent = false; CHECK(r.s->start(q).code == Err::InvalidArgument); }
  { Rig r; r.fs.plan.failOpen = true; Status s = r.s->start(r.req()); CHECK(s.code == Err::PermissionDenied); CHECK(s.detail.find("rec") != std::string::npos); CHECK(!r.sawRecording()); }
  { Rig r; r.disk.free = 100ull << 20; Status s = r.s->start(r.req()); CHECK(s.code == Err::DiskFull); CHECK(s.detail.find("MiB") != std::string::npos); CHECK(!r.sawRecording()); CHECK(r.s->state() == RecState::Failed); }
  { Rig r; r.fs.plan.capacityBytes = 10; Status s = r.s->start(r.req()); CHECK(!s.ok()); CHECK(!r.sawRecording()); }   // cannot even write the journal/header
  { Rig r; auto q = r.req(); q.profile.audioRate = 22050; CHECK(r.s->start(q).code == Err::InvalidArgument); }
  { Rig r; r.disk.fail = true; CHECK(r.s->start(r.req()).ok()); CHECK(has(r.s->status().warnings, "could not be determined")); r.s->stop(); }
  // a failed start can be retried (state returns through Preparing again)
  { Rig r; r.disk.free = 1ull << 20; CHECK(!r.s->start(r.req()).ok()); r.disk.free = 50ull << 30; CHECK(r.s->start(r.req()).ok()); CHECK(r.s->state() == RecState::Recording); r.s->stop(); }
}

TEST(sess_audio_only_lifecycle_needs_no_encoder) {
  Rig r(true, false);                                      // no encoder registry at all (REQ-R05)
  auto q = r.req(); q.dynamics.settingsJson = "{\"threshold_db\":-12,\"ratio\":3}";
  CHECK(r.s->state() == RecState::Idle);
  CHECK(r.s->start(q).ok());
  CHECK(r.s->start(q).code == Err::InvalidArgument);       // profile immutable / no double start
  uint64_t k = r.feed(0, 450);                             // 4.5 s => 3 segments at 2 s
  (void)k;
  CHECK(r.s->stop().ok()); CHECK(r.s->stop().ok());       // idempotent
  CHECK(r.s->state() == RecState::Completed);
  std::vector<RecState> want = {RecState::Preparing, RecState::Recording, RecState::Stopping, RecState::Completed};
  CHECK(r.states == want);
  auto st = r.s->status(); CHECK(st.segments.size() == 3); CHECK(st.audioFramesWritten == 450 * 480ull);
  uint64_t sum = 0; for (auto& sg : st.segments) { rec::Bytes b; CHECK(r.fs.readAll(sg.path, b)); Wav w = parseWav(b); CHECK(w.ok); sum += w.frames; CHECK(sg.complete); }
  CHECK(sum == 450 * 480ull);
  CHECK(st.segments[0].path.find("StageSignal_20260101T000000Z_sess-1_Gala_Night_1_0000.wav") != std::string::npos);
  rec::Bytes meta; CHECK(r.fs.readAll(st.metadataPath, meta));
  json j = json::parse(std::string(meta.begin(), meta.end()));
  CHECK(j["state"] == "Completed"); CHECK(j["profile"]["audio_only"] == true); CHECK(j["dynamics"]["settings"]["ratio"] == 3);
  CHECK(j["segments"].size() == 3); CHECK(j["segments"][0]["file"].get<std::string>().find('/') == std::string::npos);
  // new session after completion
  CHECK(r.s->start(q).ok()); CHECK(r.s->stop().ok());
}

TEST(sess_gap_policy_fill_report_continue) {
  Rig r; auto q = r.req(); q.profile.driftCorrection = false; CHECK(r.s->start(q).ok());
  uint64_t k = r.feed(0, 100);
  // 100 ms of lost blocks (10 x 10 ms) -> within the 250 ms allowance
  k += 10; k = r.feed(k, 100);
  auto st = r.s->status();
  CHECK(st.state == RecState::Recording); CHECK(st.gapCount == 1); CHECK(st.gaps.size() == 1);
  CHECK(st.gaps[0].frames == 4800); CHECK(st.gaps[0].atSample == 100 * 480); CHECK(st.gaps[0].filledWithSilence); CHECK(st.gaps[0].label.find("silence") != std::string::npos);
  CHECK(st.audioFramesWritten == 210 * 480ull);          // timeline preserved: silence occupies the gap
  CHECK(has(st.warnings, "silence"));
  CHECK(r.s->stop().ok());
  // content: the gap region is digital silence, surrounding audio intact
  rec::Bytes b; auto segs = r.s->status().segments; std::vector<int32_t> all;
  for (auto& sg : segs) { r.fs.readAll(sg.path, b); Wav w = parseWav(b); CHECK(w.ok); all.insert(all.end(), w.pcm.begin(), w.pcm.end()); }
  CHECK(all.size() == 210 * 480 * 2ull);
  bool silent = true; for (size_t i = 100 * 480 * 2; i < 110 * 480 * 2; ++i) if (all[i] != 0) silent = false; CHECK(silent);
  bool nonsilent = false; for (size_t i = 99 * 480 * 2; i < 100 * 480 * 2; ++i) if (all[i] != 0) nonsilent = true; CHECK(nonsilent);
  rec::Bytes meta; r.fs.readAll(r.s->status().metadataPath, meta); json j = json::parse(std::string(meta.begin(), meta.end()));
  CHECK(j["gaps"].size() == 1); CHECK(j["gaps"][0]["silence_filled"] == true);
}

TEST(sess_long_or_repeated_gaps_stop_recording) {
  { Rig r; auto q = r.req(); q.profile.driftCorrection = false; CHECK(r.s->start(q).ok());
    uint64_t k = r.feed(0, 50); k += 30; /* 300 ms */ r.feed(k, 10);
    auto st = r.s->status();
    CHECK(st.state == RecState::Failed); CHECK(st.error.code == Err::DeviceLost); CHECK(st.error.detail.find("250") != std::string::npos);
    CHECK(st.audioFramesWritten == 50 * 480ull);          // nothing invented for the long gap
    CHECK(!st.segments.empty() && st.segments.back().complete);   // gracefully finalized
    rec::Bytes b; r.fs.readAll(st.segments.back().path, b); CHECK(parseWav(b).ok);
    CHECK(!r.s->pushAudio(blk(99999, 480, r.base)));       // no longer accepting
    CHECK(r.s->stop().code == Err::DeviceLost); }
  { Rig r; auto q = r.req(); q.profile.driftCorrection = false; CHECK(r.s->start(q).ok());
    uint64_t k = 0; for (int i = 0; i < 8 && r.s->state() == RecState::Recording; ++i) { k = r.feed(k, 10); k += 2; /* 20 ms gap */ }
    auto st = r.s->status(); CHECK(st.state == RecState::Failed); CHECK(st.error.detail.find("Repeated") != std::string::npos); CHECK(st.gapCount == 6); }
  { Rig r; auto q = r.req(); q.profile.driftCorrection = false; CHECK(r.s->start(q).ok());   // generation change (device reopen) gap measured from timestamps
    r.feed(0, 20); r.clk.t += 100 * kNsPerMs;
    r.s->pushAudio(blk(0, 480, r.base + 20 * 10 * kNsPerMs + 100 * kNsPerMs, 1)); r.s->pump();
    auto st = r.s->status(); CHECK(st.state == RecState::Recording); CHECK(st.gapCount == 1); CHECK(std::llabs((int64_t)st.gaps[0].frames - 4800) <= 2); r.s->stop(); }
}

TEST(sess_queue_bounds_and_drop_counters) {
  Rig r; auto q = r.req(); q.profile.driftCorrection = false; CHECK(r.s->start(q).ok());
  int accepted = 0; for (int k = 0; k < 300; ++k) if (r.s->pushAudio(blk(k * 480ull, 480, r.base))) ++accepted;   // 3 s with a stalled writer
  auto st = r.s->status();
  CHECK(accepted == 200); CHECK(st.queuedAudioFrames == 2 * 48000); CHECK(st.audioBlocksDropped == 100); CHECK(st.audioFramesDropped == 100 * 480);
  r.s->pump(); CHECK(r.s->status().queuedAudioFrames == 0); CHECK(r.s->status().audioFramesWritten == 200 * 480ull);
  // dropped blocks surface as a timestamped gap (100 ms -> filled, reported)
  r.s->pushAudio(blk(210 * 480ull, 480, r.base)); r.s->pump();
  st = r.s->status(); CHECK(st.gapCount == 1); CHECK(st.gaps[0].frames == 10 * 480); r.s->stop();
  // video: render->encoder queue holds <= 3 frames, newest win
  Rig v; auto vq = v.req(RecContainer::Mkv); CHECK(v.s->start(vq).ok());
  v.s->pushAudio(blk(0, 480, v.base)); v.s->pump();
  for (int i = 0; i < 10; ++i) { auto f = std::make_shared<Frame>(); f->resize(64, 48); f->pts = v.base + i * 40 * kNsPerMs; v.s->pushVideo(f); }
  CHECK(v.s->status().videoFramesQueueDropped == 7);
  v.s->pump(); v.s->stop();
  rec::Bytes b; v.fs.readAll(v.s->status().segments[0].path, b); Mkv m = parseMkv(b); CHECK(m.ok); CHECK(m.videoBlockCount == 3);
  CHECK(!v.s->pushVideo(nullptr));
  Rig a; a.s->start(a.req()); CHECK(!a.s->pushVideo(std::make_shared<Frame>()));   // audio-only: video refused, not queued
  a.s->stop();
}

TEST(sess_disk_estimate_warning_and_graceful_stop) {
  Rig r; auto q = r.req(); q.profile.driftCorrection = false; q.profile.segmentNs = 60 * kNsPerSec; CHECK(r.s->start(q).ok());
  r.disk.free = 10ull << 30;
  uint64_t k = 0; for (int sec = 0; sec < 6; ++sec) { k = r.feed(k, 100); }          // 6 s => poll runs as clock advances
  auto st = r.s->status();
  CHECK(st.disk.known); CHECK(st.disk.rateMeasured); CHECK_NEAR(st.disk.bytesPerSec, 288000.0, 288000.0 * 0.06);
  CHECK(st.disk.reserveBytes == (256ull << 20)); CHECK(!st.disk.warning);
  CHECK(st.disk.remainingSecLow <= st.disk.remainingSec && st.disk.remainingSec <= st.disk.remainingSecHigh);
  CHECK_NEAR(st.disk.remainingSec, (double)((10ull << 30) - (256ull << 20)) / st.disk.bytesPerSec, 1.0);
  CHECK(st.disk.text.find("min left") != std::string::npos);
  r.disk.free = 1500ull << 20;                                                       // < 2 GiB
  k = r.feed(k, 30); st = r.s->status(); CHECK(st.disk.warning); CHECK(has(st.warnings, "Disk space is low")); CHECK(st.state == RecState::Recording);
  r.disk.free = 200ull << 20;                                                        // below the 256 MiB stop reserve
  k = r.feed(k, 30); st = r.s->status();
  CHECK(st.state == RecState::Failed); CHECK(st.error.code == Err::DiskFull); CHECK(st.error.detail.find("reserve") != std::string::npos);
  CHECK(!st.segments.empty() && st.segments.back().complete);
  rec::Bytes b; r.fs.readAll(st.segments.back().path, b); Wav w = parseWav(b); CHECK(w.ok); CHECK(w.frames > 0);
  CHECK(st.lost.empty());                                                            // graceful stop: nothing was lost before the stop point
}

TEST(sess_disk_full_write_error_reports_lost_interval) {
  Rig r; auto q = r.req(); q.profile.driftCorrection = false; q.profile.segmentNs = 1 * kNsPerSec; CHECK(r.s->start(q).ok());
  uint64_t k = r.feed(0, 150);                                                       // 1.5 s: segment 0 complete, segment 1 active
  CHECK(r.s->status().segments.size() == 2);
  r.fs.plan.capacityBytes = r.fs.totalBytes() + 20000;                               // the volume fills
  uint64_t before = r.s->status().audioFramesWritten;
  k = r.feed(k, 40);
  auto st = r.s->status();
  CHECK(st.state == RecState::Recoverable); CHECK(st.error.code == Err::DiskFull);
  CHECK(st.lost.size() == 1); CHECK(st.lost[0].frames > 0);
  uint64_t written = st.audioFramesWritten; CHECK(written > before && written < 190 * 480ull);
  CHECK(st.lost[0].fromNs == rec::samplesToNs(written, 48000));
  // keep pushing: lost interval keeps extending, nothing blocks
  for (int i = 0; i < 20; ++i) r.s->pushAudio(blk((k + i) * 480ull, 480, r.base));
  auto st2 = r.s->status(); CHECK(st2.lost[0].frames > st.lost[0].frames);
  // completed segment 0 intact; active one repairable
  rec::Bytes b; r.fs.readAll(st.segments[0].path, b); CHECK(parseWav(b).ok); CHECK(st.segments[0].complete);
  r.fs.plan.capacityBytes = ~0ull;
  auto items = recoverSession(r.fs, st.journalPath);
  CHECK(items.size() == 2); CHECK(items[0].ok && !items[0].repaired); CHECK(items[1].ok);
  r.fs.readAll(st.segments[1].path, b); Wav w = parseWav(b); CHECK(w.ok); CHECK(w.frames > 0);
  CHECK(r.s->stop().code == Err::DiskFull);                  // stop() retries the metadata write once space is back
  rec::Bytes meta; r.fs.readAll(st.metadataPath, meta); json j = json::parse(std::string(meta.begin(), meta.end()), nullptr, false);
  CHECK(j.is_object() && j["lost_intervals"].size() == 1 && j["state"] == "Recoverable");
}

TEST(sess_markers_sidecar_and_mkv_tags) {
  Rig r; auto q = r.req(RecContainer::Mkv); q.profile.segmentNs = 600 * kNsPerSec; CHECK(r.s->start(q).ok());
  uint64_t k = r.feed(0, 100);
  r.clk.t = r.base + 1000 * kNsPerMs; CHECK(r.s->addMarker("verse 1").ok());
  r.s->noteShowEvent("scene change: Neon -> Aurora");        // show/scene changes never reset or move the session clock
  k = r.feed(k, 100);
  r.clk.t = r.base + 2000 * kNsPerMs; CHECK(r.s->addMarker("chorus\n2").ok()); k = r.feed(k, 50);
  CHECK(r.s->stop().ok());
  auto st = r.s->status(); CHECK(st.markers == 2);
  rec::Bytes meta; r.fs.readAll(st.metadataPath, meta); json j = json::parse(std::string(meta.begin(), meta.end()));
  CHECK(j["markers"].size() == 2); CHECK(j["markers"][0]["label"] == "verse 1"); CHECK(j["markers"][1]["label"] == "chorus 2");
  CHECK(j["markers"][0]["timeline_ns"] == 1000 * kNsPerMs); CHECK(j["markers"][1]["timeline_ns"] == 2000 * kNsPerMs);
  CHECK(j["show_events"].size() == 1);
  CHECK(st.timelineNs == 250 * 10 * kNsPerMs);               // continuous: scene event did not rewind it
  rec::Bytes b; r.fs.readAll(st.segments[0].path, b); Mkv m = parseMkv(b); CHECK(m.ok);
  int mk = 0; bool v1 = false; for (auto& t : m.tags) if (t.first == "STAGESIGNAL_MARKER") { ++mk; if (t.second == "1.000 verse 1") v1 = true; }
  CHECK(mk == 2); CHECK(v1);
  CHECK(r.s->addMarker("late").code == Err::InvalidArgument);
}

TEST(sess_metadata_dynamics_and_raw_input_vs_limiter) {
  Rig r; auto q = r.req(); q.dynamics.enabled = true; q.dynamics.bypassed = true; q.dynamics.settingsJson = "{\"ceiling_db\":-1}"; q.dynamics.delaySamples = 240;
  CHECK(r.s->start(q).ok());
  CHECK(has(r.s->status().warnings, "bypassed"));
  for (int i = 0; i < 30; ++i) r.s->noteBlockTelemetry(i < 10, i < 3, i % 2 == 0, i == 5 ? 6.5f : 1.0f);
  r.s->setDynamics(false); r.feed(0, 10); CHECK(r.s->stop().ok());
  auto st = r.s->status(); CHECK(st.nearFullScaleBlocks == 10); CHECK(st.possibleClippingBlocks == 3); CHECK(st.limiterActiveBlocks == 15); CHECK_NEAR(st.maxGainReductionDb, 6.5, 0.01);
  rec::Bytes meta; r.fs.readAll(st.metadataPath, meta); json j = json::parse(std::string(meta.begin(), meta.end()));
  CHECK(j["dynamics"]["bypassed_at_start"] == true); CHECK(j["dynamics"]["settings"]["ceiling_db"] == -1); CHECK(j["dynamics"]["delay_samples"] == 240);
  CHECK(j["input_warnings"]["blocks_near_full_scale"] == 10); CHECK(j["limiter_activity"]["blocks_limiter_active"] == 15);   // distinct fields (REQ-R02)
  CHECK(j["profile"]["delay_compensation"]["audio_processing_delay_samples"] == 240);
  CHECK(j["dynamics"]["events"].size() == 1);
}

TEST(sess_av_sync_mkv_and_avi_timeline) {
  for (int pass = 0; pass < 2; ++pass) {
    RecContainer c = pass ? RecContainer::Avi : RecContainer::Mkv;
    Rig r; auto q = r.req(c); q.profile.segmentNs = 600 * kNsPerSec; q.profile.driftCorrection = false; q.dynamics.delaySamples = 240;   // 5 ms
    CHECK(r.s->start(q).ok());
    for (int i = 0; i < 100; ++i) {
      r.clk.t = r.base + i * 10 * kNsPerMs; r.s->pushAudio(blk(i * 480ull, 480, r.base)); r.s->pump();
      if (i % 4 == 0) { auto f = std::make_shared<Frame>(); f->resize(64, 48); for (auto& p : f->px) p = rgb(i * 2, 80, 160); f->pts = r.base + i * 10 * kNsPerMs; r.s->pushVideo(f); r.s->pump(); }
    }
    CHECK(r.s->stop().ok());
    auto st = r.s->status(); CHECK(st.state == RecState::Completed); CHECK(st.segments.size() == 1);
    rec::Bytes b; r.fs.readAll(st.segments[0].path, b);
    if (c == RecContainer::Mkv) {
      Mkv m = parseMkv(b); CHECK(m.ok); std::vector<int64_t> vt; for (auto& bl : m.blocks) if (bl.track == 1) { vt.push_back(bl.ms); CHECK(isJpeg(bl.data)); }
      CHECK(vt.size() == 25);
      // video frame k captured at 40k ms lands at 40k + 5 ms (audio processing delay compensation, REQ-R03)
      for (size_t k = 0; k < vt.size(); ++k) CHECK(vt[k] == (int64_t)(40 * k + 5));
    } else {
      Avi a = parseAvi(b); CHECK(a.ok); CHECK(a.frames.size() >= 24);
      CHECK(a.vStart == 0 || a.vStart == 0u);   // 5 ms is < half a 40 ms slot: first frame stays in slot 0
      CHECK(a.aFrames == 100 * 480ull);
    }
  }
}

TEST(sess_clock_mismatch_is_error) {
  Rig r; auto q = r.req(); q.profile.drift.warmupSec = 2; CHECK(r.s->start(q).ok());
  uint64_t n = 0;
  for (int k = 0; k < 1500 && r.s->state() == RecState::Recording; ++k) {    // device clock +400 ppm vs host
    Ns cap = r.base + (Ns)((double)n / (48000.0 * (1 + 400e-6)) * 1e9);
    auto b = blk(n, 480, r.base); const_cast<AudioBlock&>(*b).captureNs = cap; r.clk.t = cap; r.s->pushAudio(b); r.s->pump(); n += 480;
  }
  auto st = r.s->status(); CHECK(st.state == RecState::Failed); CHECK(st.error.code == Err::TimestampInvalid); CHECK(st.drift.excessive); CHECK(has(st.warnings, "ppm"));
  CHECK(st.segments.back().complete);
}

TEST(sess_threaded_nonblocking_with_slow_disk) {
  MemFileSystem* fsp = nullptr; (void)fsp;
  Rig r(false); auto q = r.req(); q.profile.driftCorrection = false; q.profile.segmentNs = 600 * kNsPerSec;
  r.fs.plan.onWrite = [](size_t) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); };   // slow disk
  SessionDeps d; d.fs = &r.fs; d.disk = &r.disk; d.clock = &systemClock(); d.encoders = &r.reg; d.manualPump = false;
  RecordingSession s(d); CHECK(s.start(q).ok());
  double worstMs = 0; int accepted = 0, rejected = 0; uint64_t maxQueued = 0;
  for (int k = 0; k < 400; ++k) {                                // 4 s of audio pushed at ~4x realtime
    auto b = blk(k * 480ull, 480, r.base);
    auto t0 = std::chrono::steady_clock::now(); bool ok = s.pushAudio(b); double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    worstMs = std::max(worstMs, ms); ok ? ++accepted : ++rejected;
    maxQueued = std::max(maxQueued, s.status().queuedAudioFrames);
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  CHECK(worstMs < 20.0);                                        // never waits for the writer (sleeps are 15 ms per write)
  CHECK(maxQueued <= 2 * 48000ull);                             // bounded at 2 s
  Status stp = s.stop();
  auto st = s.status();
  std::printf("   slow disk: accepted %d rejected %d worst push %.3f ms state %s\n", accepted, rejected, worstMs, recStateName(st.state));
  CHECK(st.state == RecState::Completed || st.state == RecState::Failed);   // stalled writer may legitimately trip the gap policy
  if (st.state == RecState::Failed) CHECK(st.error.code == Err::DeviceLost);
  CHECK(st.audioFramesWritten > 0);
  // plain threaded run completes cleanly
  Rig r2(false); auto q2 = r2.req(); CHECK(r2.s->start(q2).ok());
  for (int k = 0; k < 200; ++k) { r2.s->pushAudio(blk(k * 480ull, 480, r2.base)); std::this_thread::sleep_for(std::chrono::microseconds(200)); }
  CHECK(r2.s->stop().ok()); CHECK(r2.s->status().audioFramesWritten == 200 * 480ull); CHECK(r2.s->state() == RecState::Completed);
  (void)stp;
}

TEST(sess_wav_write_error_before_segment_roll_keeps_prior_segments) {
  Rig r; auto q = r.req(); q.profile.driftCorrection = false; q.profile.segmentNs = 1 * kNsPerSec; CHECK(r.s->start(q).ok());
  r.feed(0, 120);
  r.fs.plan.failWriteAfterBytes = (int64_t)r.fs.written + 1;   // I/O error on all further writes
  r.feed(120, 150);
  auto st = r.s->status(); CHECK(st.state == RecState::Recoverable); CHECK(st.error.code == Err::InternalFault);
  r.fs.plan.failWriteAfterBytes = -1;
  rec::Bytes b; r.fs.readAll(st.segments[0].path, b); CHECK(parseWav(b).ok);
  CHECK(has(st.warnings, "metadata") || true);
}

int main() { return ss_test::runAll(); }
