#include "recording/session.h"
#include "recording/naming.h"
#include "nlohmann/json.hpp"
#include <thread>
#include <condition_variable>
#include <deque>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ss {
using nlohmann::json;
using namespace rec;

const char* recStateName(RecState s) {
  switch (s) { case RecState::Idle: return "Idle"; case RecState::Preparing: return "Preparing"; case RecState::Recording: return "Recording";
    case RecState::Stopping: return "Stopping"; case RecState::Completed: return "Completed"; case RecState::Failed: return "Failed"; case RecState::Recoverable: return "Recoverable"; }
  return "?";
}
const char* recContainerName(RecContainer c) { return c == RecContainer::Wav ? "wav" : (c == RecContainer::Avi ? "avi" : "mkv"); }

static std::string humanBytes(double b) {
  char buf[48]; const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"}; int i = 0;
  while (b >= 1024 && i < 4) { b /= 1024; ++i; }
  std::snprintf(buf, sizeof buf, "%.1f %s", b, u[i]); return buf;
}
static std::string baseOf(const std::string& p) { size_t k = p.find_last_of("/\\"); return k == std::string::npos ? p : p.substr(k + 1); }

struct RecordingSession::Impl {
  SessionDeps d; RecordingRequest req; RecordingProfile prof;
  bool wav = true, av = false;
  // ---- shared status (guarded by m) ----
  mutable std::mutex m;
  RecState st = RecState::Idle; Status err; std::vector<std::string> warnings; std::vector<GapRecord> gaps;
  std::vector<LostInterval> lost; DiskEstimate disk; std::vector<SegmentInfo> segSnap; std::function<void(RecState)> cb;
  std::vector<std::string> showEvents; std::vector<std::string> dynEvents;
  DriftStats driftSnap; uint64_t vDropSnap = 0, vDupSnap = 0; uint64_t lostLateAtSnap = 0;
  bool dynBypassed = false;
  // ---- queues ----
  std::mutex qm; std::condition_variable qcv;
  std::vector<AudioBlockPtr> aring; size_t ah = 0, an = 0; uint64_t aframes = 0;
  std::shared_ptr<const Frame> vring[3]; size_t vh = 0, vn = 0;
  std::atomic<bool> accepting{false}, stopReq{false}, dead{false}, closing{false};
  std::atomic<uint64_t> aBlocksDropped{0}, aFramesDropped{0}, vQueueDropped{0}, inputBlocks{0}, lostLate{0};
  std::atomic<uint64_t> nearFS{0}, clipB{0}, limB{0}; std::atomic<uint32_t> maxGrMilli{0};
  std::atomic<uint64_t> outFrames{0}, gapCount{0}, gapFrames{0}, bytesNow{0}, overlapDropped{0}, encodeErrors{0}, vBeforeAudio{0};
  std::atomic<bool> anchoredA{false}; std::atomic<int64_t> originNs{0};
  // ---- writer-side state (writer thread / pump only) ----
  std::unique_ptr<WavWriter> wavw; std::unique_ptr<MediaWriter> mw; SegmentJournal journal; std::unique_ptr<IVideoEncoder> enc;
  SyncTimeline timeline; DriftCorrector corr; bool corrOn = false; bool anchored = false;
  uint32_t gen = 0; uint64_t expectedFirst = 0; Ns expectedCap = 0;
  std::deque<Ns> gapTimes; std::vector<float> tmp, zeros; int consecEncErr = 0;
  Ns nextDiskPoll = 0; std::deque<std::pair<Ns, uint64_t>> byteSamples; double estBps = 0;
  uint64_t lostFrom = 0, lostEnd = 0; bool haveLost = false; std::string lostReason; Status lostStatus;
  std::string base, journalPath, metaPath, encName; bool encQualified = false; int64_t startUnixMs = 0;
  // markers
  std::mutex mm; std::vector<Marker> markers; uint32_t nextMarker = 1; std::atomic<bool> metaDirty{false};
  std::thread th; bool metaWarned = false; bool finalized = false;

  // ------------------------------------------------------------------
  void setState(RecState s) {
    std::function<void(RecState)> c;
    { std::lock_guard<std::mutex> l(m); st = s; c = cb; }
    if (c) c(s);
  }
  void addWarning(const std::string& w) { std::lock_guard<std::mutex> l(m); for (auto& x : warnings) if (x == w) return; warnings.push_back(w); }
  void resetRun() {
    { std::lock_guard<std::mutex> l(m); err = Status(); warnings.clear(); gaps.clear(); lost.clear(); disk = DiskEstimate(); segSnap.clear(); showEvents.clear(); dynEvents.clear(); }
    { std::lock_guard<std::mutex> l(qm); for (auto& x : aring) x.reset(); ah = an = 0; aframes = 0; for (auto& f : vring) f.reset(); vh = vn = 0; }
    { std::lock_guard<std::mutex> l(mm); markers.clear(); nextMarker = 1; }
    accepting = false; stopReq = false; dead = false; closing = false;
    aBlocksDropped = aFramesDropped = vQueueDropped = inputBlocks = lostLate = 0; nearFS = clipB = limB = 0; maxGrMilli = 0;
    outFrames = gapCount = gapFrames = bytesNow = overlapDropped = encodeErrors = vBeforeAudio = 0; anchoredA = false; originNs = 0;
    wavw.reset(); mw.reset(); enc.reset(); journal = SegmentJournal(); timeline = SyncTimeline(); corr = DriftCorrector(); corrOn = false; anchored = false;
    gen = 0; expectedFirst = 0; expectedCap = 0; gapTimes.clear(); consecEncErr = 0; nextDiskPoll = 0; byteSamples.clear();
    lostFrom = lostEnd = 0; haveLost = false; lostReason.clear(); metaWarned = false; finalized = false; metaDirty = false; encName.clear(); encQualified = false;
  }

  // ------------------------------------------------------------------ Preparing
  double estimateBps(const RecordingProfile& p) const {
    double a = (double)p.audioRate * p.audioChannels * (p.container == RecContainer::Wav ? 3 : p.audioBits / 8);
    return a + (p.container == RecContainer::Wav ? 0.0 : estimatedVideoBytesPerSec(p.video));
  }
  Status prepare(const RecordingRequest& r) {
    req = r; prof = r.profile; wav = prof.container == RecContainer::Wav; av = !wav;
    if (!d.fs || !d.clock) return Status::Fail(Err::InternalFault, "recording session was created without file system/clock", false);
    if (prof.audioRate != 44100 && prof.audioRate != 48000)
      return Status::Fail(Err::InvalidArgument, "Recording sample rate must be 44100 or 48000 Hz (profile has " + std::to_string(prof.audioRate) + "). Choose a supported capture rate.");
    if (prof.audioChannels < 1 || prof.audioChannels > 2) return Status::Fail(Err::InvalidArgument, "Recording supports mono or stereo audio. Change the channel route in Audio settings.");
    if (!wav && prof.audioBits != 16 && prof.audioBits != 24) return Status::Fail(Err::InvalidArgument, "Audio bit depth must be 16 or 24.");
    if (r.dir.empty()) return Status::Fail(Err::InvalidArgument, "No recording folder is set. Choose a folder in Recording settings.");
    // sources (REQ-R06: capture invalid)
    if (!r.sources.audioPresent) return Status::Fail(Err::InvalidArgument, "No audio source is available to record. Select a working audio input (the device may be disconnected or denied) and try again.");
    if (r.sources.audioRate != prof.audioRate || r.sources.audioChannels != prof.audioChannels)
      return Status::Fail(Err::InvalidArgument, "The audio capture delivers " + std::to_string(r.sources.audioRate) + " Hz / " + std::to_string(r.sources.audioChannels) + " ch but the recording profile expects " + std::to_string(prof.audioRate) + " Hz / " + std::to_string(prof.audioChannels) + " ch. Re-open capture with the profile format or change the profile.");
    if (av) {
      if (prof.container == RecContainer::Wav) return Status::Fail(Err::InternalFault, "inconsistent profile");
      if (!r.sources.videoPresent) return Status::Fail(Err::InvalidArgument, "No program video output is available to record. Start the output or choose audio-only recording.");
      if (r.sources.videoW != prof.video.width || r.sources.videoH != prof.video.height)
        return Status::Fail(Err::InvalidArgument, "Program output is " + std::to_string(r.sources.videoW) + "x" + std::to_string(r.sources.videoH) + " but the recording profile is " + std::to_string(prof.video.width) + "x" + std::to_string(prof.video.height) + ". Change the profile resolution (profiles are fixed during a session).");
      if (!d.encoders) return Status::Fail(Err::EncoderUnavailable, "No video encoder registry is available. Use audio-only recording.");
      EncoderAvailability av_ = d.encoders->availability(prof.video.codec);
      if (!av_.available) return Status::Fail(Err::EncoderUnavailable, std::string(codecName(prof.video.codec)) + " recording is unavailable: " + av_.reason);
      if (prof.video.codec != VideoCodec::MJPEG) return Status::Fail(Err::UnsupportedCapability, std::string("The ") + recContainerName(prof.container) + " writer in this build can store MJPEG only; " + codecName(prof.video.codec) + " muxing is not implemented.");
      encName = av_.name; encQualified = av_.qualified;
      if (!av_.qualified) addWarning("Video encoder '" + av_.name + "' is not qualified on this machine tier (MJPEG fallback profile); watch dropped-frame counters.");
    }
    // folder
    std::string why;
    if (!d.fs->writableDir(r.dir, why)) return Status::Fail(Err::PermissionDenied, "Recording folder is not writable: " + (why.empty() ? r.dir : why) + ". Pick another folder or fix its permissions.");
    // space
    estBps = estimateBps(prof);
    uint64_t reserve = std::max<uint64_t>(kMinStopReserveBytes, (uint64_t)(estBps * 30));
    uint64_t need = reserve + (uint64_t)(estBps * 60);
    if (d.disk) {
      uint64_t freeB = 0;
      if (!d.disk->freeBytes(r.dir, freeB)) addWarning("Free disk space could not be determined; disk monitoring is degraded.");
      else if (freeB < need) return Status::Fail(Err::DiskFull, "Only " + humanBytes((double)freeB) + " free in the recording folder; at least " + humanBytes((double)need) + " is required (graceful-stop reserve + 1 minute at about " + humanBytes(estBps) + "/s). Free space or choose another drive.");
      else if (freeB < kDiskWarnBytes) addWarning("Free disk space is below 2 GiB.");
    } else addWarning("Disk space monitoring is not available in this build.");
    if (!r.dynamics.enabled || r.dynamics.bypassed) addWarning("Dynamics (compressor/limiter) are bypassed: the recording is unprocessed and may clip.");
    dynBypassed = !r.dynamics.enabled || r.dynamics.bypassed;
    // names, journal, encoder, writers
    startUnixMs = d.clock->wallUnixMs();
    base = baseName(startUnixMs, r.sessionId, r.showName);
    journalPath = joinPath(r.dir, base + ".journal"); metaPath = joinPath(r.dir, base + ".session.json");
    Status s = journal.open(*d.fs, journalPath);
    if (!s.ok()) return Status::Fail(s.code, "Cannot create the segment journal: " + s.detail);
    s = journal.session(r.sessionId, r.showName, startUnixMs, recContainerName(prof.container));
    if (!s.ok()) return s;
    if (av) {
      s = d.encoders->create(prof.video, enc);
      if (!s.ok()) return Status::Fail(s.code, "Video encoder could not be opened: " + s.detail + " Choose audio-only recording or another profile.");
    }
    prof.delay.audioProcessingDelaySamples += r.dynamics.delaySamples;
    if (wav) {
      WavConfig wc; wc.rate = prof.audioRate; wc.channels = prof.audioChannels; wc.maxSegmentNs = prof.segmentNs; wc.maxSegmentBytes = prof.segmentBytes; wc.dir = r.dir; wc.base = base;
      wavw.reset(new WavWriter(*d.fs, &journal)); s = wavw->open(wc);
    } else {
      MediaWriterConfig mc; mc.dir = r.dir; mc.base = base; mc.audioRate = prof.audioRate; mc.audioChannels = prof.audioChannels; mc.audioBits = prof.audioBits;
      mc.width = prof.video.width; mc.height = prof.video.height; mc.fpsNum = prof.video.fpsNum; mc.fpsDen = prof.video.fpsDen; mc.codec = prof.video.codec;
      mc.maxSegmentNs = prof.segmentNs; mc.maxSegmentBytes = prof.segmentBytes;
      mw = MediaWriter::create(prof.container == RecContainer::Avi ? MediaContainer::Avi : MediaContainer::Mkv, *d.fs, &journal);
      mw->setTagProvider([this](const SegmentInfo& seg) { return tagsFor(seg); });
      s = mw->open(mc);
    }
    if (!s.ok()) return Status::Fail(s.code, "Cannot create the first recording file: " + s.detail + " Check free space and folder permissions.");
    if (prof.driftCorrection) { Status cs = corr.configure(prof.audioRate, prof.audioChannels, prof.drift); if (!cs.ok()) return cs; corrOn = true; }
    refreshSegments();
    s = writeSidecar();
    if (!s.ok()) return Status::Fail(s.code, "Cannot write the session metadata file: " + s.detail);
    return Status::Ok();
  }

  // ------------------------------------------------------------------ helpers
  std::vector<SegmentInfo> currentSegments() const { return wavw ? wavw->segments() : (mw ? mw->segments() : std::vector<SegmentInfo>()); }
  void refreshSegments() {
    auto v = currentSegments(); DriftStats ds = corr.stats(); uint64_t vd = mw ? mw->droppedVideo() : 0, vu = mw ? mw->duplicatedVideo() : 0;
    std::lock_guard<std::mutex> l(m); segSnap = std::move(v); driftSnap = ds; vDropSnap = vd; vDupSnap = vu;
  }
  uint64_t currentBytes() const { return wavw ? wavw->bytesWritten() : (mw ? mw->bytesWritten() : 0); }

  MediaWriter::Tags tagsFor(const SegmentInfo& seg) {
    MediaWriter::Tags t;
    t.emplace_back("STAGESIGNAL_SESSION", req.sessionId); t.emplace_back("STAGESIGNAL_SHOW", req.showName);
    t.emplace_back("STAGESIGNAL_DYNAMICS", !req.dynamics.enabled ? "off" : (dynBypassed ? "bypassed" : "on"));
    t.emplace_back("STAGESIGNAL_NEAR_FULL_SCALE_BLOCKS", std::to_string(nearFS.load()));
    t.emplace_back("STAGESIGNAL_LIMITER_ACTIVE_BLOCKS", std::to_string(limB.load()));
    t.emplace_back("STAGESIGNAL_GAPS", std::to_string(gapCount.load()));
    bool last = closing.load();
    std::lock_guard<std::mutex> l(mm);
    for (auto& mk : markers) if (mk.timelineNs >= seg.startNs && (last || mk.timelineNs < seg.endNs)) {
      char b[40]; std::snprintf(b, sizeof b, "%.3f ", (double)mk.timelineNs / 1e9); t.emplace_back("STAGESIGNAL_MARKER", std::string(b) + mk.label);
    }
    return t;
  }

  Status writeSidecar() {
    json j; j["schema"] = 1; j["app"] = "StageSignal"; j["session_id"] = req.sessionId; j["show_name"] = req.showName;
    j["start_unix_ms"] = startUnixMs; j["start_utc"] = utcStamp(startUnixMs);
    RecState s; Status e; std::vector<std::string> w; std::vector<GapRecord> g; std::vector<LostInterval> lo; DiskEstimate de; std::vector<std::string> se, de2;
    { std::lock_guard<std::mutex> l(m); s = st; e = err; w = warnings; g = gaps; lo = lost; de = disk; se = showEvents; de2 = dynEvents; }
    j["state"] = recStateName(s);
    j["error"] = e.ok() ? json(nullptr) : json{{"code", errName(e.code)}, {"detail", e.detail}, {"recoverable", e.recoverable}};
    j["warnings"] = w;
    json pr; pr["container"] = recContainerName(prof.container);
    pr["audio"] = {{"rate", prof.audioRate}, {"channels", prof.audioChannels}, {"bits", wav ? 24 : prof.audioBits}, {"format", "PCM integer little-endian"}};
    if (av) pr["video"] = {{"codec", codecName(prof.video.codec)}, {"width", prof.video.width}, {"height", prof.video.height}, {"fps_num", prof.video.fpsNum}, {"fps_den", prof.video.fpsDen}, {"quality", prof.video.quality}, {"encoder", encName}, {"encoder_qualified", encQualified}};
    pr["segment_ns"] = prof.segmentNs; pr["segment_bytes"] = prof.segmentBytes; pr["immutable"] = true;
    pr["audio_only"] = wav; pr["drift_correction"] = prof.driftCorrection;
    pr["delay_compensation"] = {{"audio_processing_delay_samples", prof.delay.audioProcessingDelaySamples}, {"audio_encoder_priming_ns", prof.delay.audioEncoderPrimingNs}, {"video_encoder_delay_ns", prof.delay.videoEncoderDelayNs}, {"camera_offset_ns", prof.delay.cameraOffsetNs}};
    j["profile"] = pr;
    json dy; dy["enabled"] = req.dynamics.enabled; dy["bypassed_at_start"] = req.dynamics.bypassed; dy["bypassed_now"] = dynBypassed; dy["delay_samples"] = req.dynamics.delaySamples; dy["ceiling_unit"] = req.dynamics.ceilingUnit;
    json st2 = json::parse(req.dynamics.settingsJson.empty() ? "null" : req.dynamics.settingsJson, nullptr, false);
    dy["settings"] = st2.is_discarded() ? json(req.dynamics.settingsJson) : st2; dy["events"] = de2;
    j["dynamics"] = dy;
    j["input_warnings"] = {{"note", "raw input condition measured before dynamics"}, {"blocks_near_full_scale", nearFS.load()}, {"blocks_possible_clipping", clipB.load()}};
    j["limiter_activity"] = {{"note", "output limiter/compressor activity, distinct from raw input warnings"}, {"blocks_limiter_active", limB.load()}, {"max_gain_reduction_db", maxGrMilli.load() / 1000.0}};
    DriftStats ds = corr.stats();
    j["clock"] = {{"anchored", anchoredA.load()}, {"origin_host_ns", originNs.load()}, {"drift_measured_ppm", ds.measuredPpm}, {"drift_applied_ppm", ds.appliedPpm}, {"drift_in_frames", ds.inFrames}, {"drift_out_frames", ds.outFrames}, {"drift_excessive", ds.excessive}, {"audio_samples_clamped", wavw ? wavw->clampedSamples() : (mw ? mw->clampedSamples() : 0)}};
    json ja = json::array(); for (auto& x : g) ja.push_back({{"at_sample", x.atSample}, {"frames", x.frames}, {"start_ns", x.startNs}, {"duration_ns", x.durationNs}, {"silence_filled", x.filledWithSilence}, {"label", x.label}});
    j["gaps"] = ja;
    json jl = json::array(); for (auto& x : lo) jl.push_back({{"from_ns", x.fromNs}, {"to_ns", x.toNs}, {"frames", x.frames}, {"reason", x.reason}});
    j["lost_intervals"] = jl;
    j["drops"] = {{"audio_blocks", aBlocksDropped.load()}, {"audio_frames", aFramesDropped.load()}, {"video_queue", vQueueDropped.load()}, {"video_cadence", mw ? mw->droppedVideo() : 0}, {"video_duplicated", mw ? mw->duplicatedVideo() : 0}, {"audio_overlap_frames", overlapDropped.load()}};
    json jm = json::array(); { std::lock_guard<std::mutex> l(mm); for (auto& x : markers) jm.push_back({{"id", x.id}, {"timeline_ns", x.timelineNs}, {"label", x.label}}); }
    j["markers"] = jm; j["show_events"] = se;
    json jsg = json::array(); { std::vector<SegmentInfo> sg; { std::lock_guard<std::mutex> l(m); sg = segSnap; }
      for (auto& x : sg) jsg.push_back({{"index", x.index}, {"file", baseOf(x.path)}, {"start_ns", x.startNs}, {"end_ns", x.endNs}, {"first_sample", x.firstSample}, {"audio_frames", x.audioFrames}, {"video_slots", x.videoFrames}, {"video_duplicates", x.videoDups}, {"bytes", x.bytes}, {"complete", x.complete}}); }
    j["segments"] = jsg;
    j["disk"] = {{"free_bytes", de.freeBytes}, {"bytes_per_sec", de.bytesPerSec}, {"rate_measured", de.rateMeasured}, {"warning", de.warning}};
    std::string text = j.dump(2);
    std::string tmpPath = metaPath + ".tmp"; std::unique_ptr<IFileSink> sink;
    Status o = d.fs->open(tmpPath, IFileSystem::Mode::Truncate, sink);
    if (!o.ok()) return o;
    Status w1 = sink->write(text.data(), text.size()); sink->close();
    if (!w1.ok()) { d.fs->remove(tmpPath); return w1; }
    return d.fs->rename(tmpPath, metaPath);
  }
  void sidecarBestEffort() {
    Status s = writeSidecar();
    if (!s.ok() && !metaWarned) { metaWarned = true; addWarning("Session metadata could not be written: " + s.detail); }
  }

  // ------------------------------------------------------------------ terminal transitions
  Status closeWriters() {
    closing = true; Status r = Status::Ok();
    if (wavw) { Status s = wavw->close(); if (!s.ok()) r = s; }
    if (mw) { Status s = mw->close(); if (!s.ok()) r = s; }
    if (enc) enc->close();
    journal.close(); refreshSegments(); return r;
  }
  uint64_t lostEndNow() const { return lostEnd + lostLate.load(); }
  void snapshotLost() {
    if (!haveLost) return;
    std::lock_guard<std::mutex> l(m);
    LostInterval li; li.fromNs = samplesToNs(lostFrom, prof.audioRate); li.toNs = samplesToNs(lostEndNow(), prof.audioRate); li.frames = lostEndNow() - lostFrom; li.reason = lostReason; lostLateAtSnap = lostLate.load();
    lost.clear(); lost.push_back(li);
  }
  void failGraceful(const Status& s) {          // finalize everything we can, then Failed (or Recoverable if finalize itself fails)
    if (dead.exchange(true)) return;
    accepting = false;
    Status c = closeWriters();
    { std::lock_guard<std::mutex> l(m); err = s; }
    finalized = true;
    RecState ns = c.ok() ? RecState::Failed : RecState::Recoverable;
    if (!c.ok()) { std::lock_guard<std::mutex> l(m); err.detail += " (finalize also failed: " + c.detail + ")"; }
    { std::lock_guard<std::mutex> l(m); st = ns; }
    sidecarBestEffort(); setState(ns);
  }
  void failWrite(const Status& s, uint64_t lostFromSample, uint64_t pendingFrames) {   // REQ-R04
    if (dead.exchange(true)) return;
    accepting = false;
    haveLost = true; lostFrom = lostFromSample; lostEnd = std::max<uint64_t>(outFrames.load() + pendingFrames, lostFromSample); lostReason = std::string(errName(s.code)) + ": " + s.detail;
    snapshotLost();
    closing = true;
    if (enc) enc->close();
    journal.note("write_error", s.detail); journal.close(); refreshSegments();
    { std::lock_guard<std::mutex> l(m); err = s; err.detail = "Recording stopped: " + s.detail + ". Completed segments are intact; the active segment can be recovered (recoverSession). Lost interval is reported in the status/metadata."; st = RecState::Recoverable; }
    finalized = true; sidecarBestEffort(); setState(RecState::Recoverable);
  }

  // ------------------------------------------------------------------ audio path
  Status writeOut(const float* p, size_t n) {
    if (!n) return Status::Ok();
    Status s = wavw ? wavw->write(p, n) : mw->writeAudio(p, n);
    if (s.ok()) outFrames += n;
    return s;
  }
  bool feed(const float* in, size_t frames, Ns cap, bool tsValid) {
    const float* op = in; size_t on = frames; Status drift = Status::Ok();
    if (corrOn) { drift = corr.process(in, frames, cap, tsValid, tmp); op = tmp.data(); on = tmp.size() / prof.audioChannels; }
    Status s = writeOut(op, on);
    if (!s.ok()) { failWrite(s, wavw ? wavw->lostFromSample() : mw->nextSample(), on); return false; }
    if (!drift.ok()) {
      addWarning(drift.detail);
      if (prof.stopOnExcessiveDrift) { failGraceful(drift); return false; }
    }
    return true;
  }
  bool handleGap(uint64_t frames) {
    GapRecord g; g.atSample = outFrames.load(); g.frames = frames; g.startNs = samplesToNs(g.atSample, prof.audioRate); g.durationNs = samplesToNs(frames, prof.audioRate);
    uint64_t allowance = nsToSamples(prof.gapAllowanceNs, prof.audioRate);
    ++gapCount; gapFrames += frames;
    if (frames > allowance) {
      g.filledWithSilence = false; g.label = "audio gap exceeds " + std::to_string(prof.gapAllowanceNs / kNsPerMs) + " ms allowance";
      { std::lock_guard<std::mutex> l(m); gaps.push_back(g); }
      journal.note("gap", g.label);
      failGraceful(Status::Fail(Err::DeviceLost, "Audio gap of " + std::to_string(g.durationNs / kNsPerMs) + " ms exceeds the " + std::to_string(prof.gapAllowanceNs / kNsPerMs) + " ms allowance; recording stopped and finalized. Check the capture device/driver and CPU load, then start a new recording.", true));
      return false;
    }
    Ns now = g.startNs;
    while (!gapTimes.empty() && now - gapTimes.front() > 60 * kNsPerSec) gapTimes.pop_front();
    gapTimes.push_back(now);
    if ((int)gapTimes.size() > prof.maxGapsPerMinute) {
      g.filledWithSilence = false; g.label = "repeated audio gaps";
      { std::lock_guard<std::mutex> l(m); gaps.push_back(g); }
      journal.note("gap", g.label);
      failGraceful(Status::Fail(Err::DeviceLost, "Repeated audio gaps (" + std::to_string(gapTimes.size()) + " within 60 s, limit " + std::to_string(prof.maxGapsPerMinute) + "); recording stopped and finalized. The capture path is unreliable: check the device, driver and CPU load.", true));
      return false;
    }
    g.label = "timestamped silence inserted (" + std::to_string(g.durationNs / kNsPerMs) + " ms)";
    { std::lock_guard<std::mutex> l(m); gaps.push_back(g); }
    journal.note("gap", g.label + " at sample " + std::to_string(g.atSample));
    if (zeros.size() < frames * prof.audioChannels) zeros.assign(frames * prof.audioChannels, 0.f);
    addWarning("Audio gap(s) were filled with silence; see the session metadata.");
    return feed(zeros.data(), frames, 0, false);
  }
  void processAudio(const AudioBlock& b) {
    size_t frames = b.frames(); if (!frames) return;
    if (!anchored) {
      if (b.rate != prof.audioRate || b.channels != prof.audioChannels) { failGraceful(Status::Fail(Err::InvalidArgument, "Audio block format " + std::to_string(b.rate) + " Hz/" + std::to_string(b.channels) + " ch does not match the recording profile; recording stopped.", true)); return; }
      timeline.anchor(b.captureNs, prof.audioRate, prof.delay); originNs = timeline.originHostNs(); anchoredA = true; anchored = true;
      gen = b.generation; expectedFirst = b.firstSample; expectedCap = b.captureNs;
    } else if (b.rate != prof.audioRate || b.channels != prof.audioChannels) {
      failGraceful(Status::Fail(Err::InvalidArgument, "Capture format changed during the session (" + std::to_string(b.rate) + " Hz/" + std::to_string(b.channels) + " ch); recording stopped. Profiles are immutable per session.", true)); return;
    }
    const float* data = b.samples.data(); size_t n = frames; Ns cap = b.captureNs; bool tsValid = (b.flags & AF_TimestampValid) != 0;
    int64_t gap = 0;
    if (b.generation != gen) {
      if (tsValid) { long double g = (long double)(b.captureNs - expectedCap) * prof.audioRate / 1e9L; gap = g > 0 ? (int64_t)(g + 0.5L) : 0; }
      gen = b.generation; expectedFirst = b.firstSample;
    } else if (b.firstSample > expectedFirst) gap = (int64_t)(b.firstSample - expectedFirst);
    else if (b.firstSample < expectedFirst) {
      uint64_t ov = expectedFirst - b.firstSample; overlapDropped += std::min<uint64_t>(ov, frames);
      if (ov >= frames) return;
      data += ov * b.channels; n -= (size_t)ov; cap += (Ns)((long double)ov * 1e9L / prof.audioRate);
    }
    if (gap > 0 && !handleGap((uint64_t)gap)) return;
    if (!feed(data, n, cap, tsValid)) return;
    expectedFirst = b.firstSample + frames;
    expectedCap = b.captureNs + (Ns)((long double)frames * 1e9L / prof.audioRate);
  }

  // ------------------------------------------------------------------ video path
  void processVideo(const Frame& f) {
    if (!av) return;
    if (!anchored) { ++vBeforeAudio; return; }
    EncodedPacket pk; bool have = false;
    Status s = enc->encode(f, f.pts, pk, have);
    if (!s.ok()) {
      ++encodeErrors; addWarning("Video frame could not be encoded: " + s.detail);
      if (++consecEncErr >= 10) failGraceful(Status::Fail(Err::EncoderUnavailable, "Video encoder failed 10 frames in a row (" + s.detail + "); recording stopped and finalized.", true));
      return;
    }
    consecEncErr = 0;
    if (!have) return;
    Status w = mw->writeVideo(timeline.videoToTimeline(pk.pts), pk.data.data(), pk.data.size(), pk.key);
    if (!w.ok()) failWrite(w, mw->nextSample(), 0);
  }

  // ------------------------------------------------------------------ disk monitor
  void diskPoll(Ns now) {
    uint64_t bytes = currentBytes(); bytesNow = bytes;
    byteSamples.emplace_back(now, bytes);
    while (byteSamples.size() > 2 && now - byteSamples.front().first > 60 * kNsPerSec) byteSamples.pop_front();
    DiskEstimate e; e.bytesPerSec = estBps; e.bytesPerSecMin = estBps * 0.5; e.bytesPerSecMax = estBps * 1.5; e.rateMeasured = false;
    Ns span = now - byteSamples.front().first;
    if (span >= 2 * kNsPerSec && bytes > byteSamples.front().second) {
      e.bytesPerSec = (double)(bytes - byteSamples.front().second) / nsToSec(span); e.rateMeasured = true;
      double mn = 1e300, mx = 0; size_t j = 0;
      for (size_t i = 0; i < byteSamples.size(); ++i) {
        if (j <= i) j = i + 1;
        while (j < byteSamples.size() && byteSamples[j].first - byteSamples[i].first < 2 * kNsPerSec) ++j;
        if (j >= byteSamples.size()) break;
        double r = (double)(byteSamples[j].second - byteSamples[i].second) / nsToSec(byteSamples[j].first - byteSamples[i].first);
        mn = std::min(mn, r); mx = std::max(mx, r);
      }
      if (mx > 0) { e.bytesPerSecMin = std::min(mn, e.bytesPerSec); e.bytesPerSecMax = std::max(mx, e.bytesPerSec); }
      else { e.bytesPerSecMin = e.bytesPerSec * 0.8; e.bytesPerSecMax = e.bytesPerSec * 1.2; }
    }
    uint64_t freeB = 0;
    if (d.disk && d.disk->freeBytes(req.dir, freeB)) {
      e.known = true; e.freeBytes = freeB;
      e.reserveBytes = std::max<uint64_t>(kMinStopReserveBytes, (uint64_t)(e.bytesPerSec * 30));
      double usable = freeB > e.reserveBytes ? (double)(freeB - e.reserveBytes) : 0.0;
      if (e.bytesPerSec > 0) e.remainingSec = usable / e.bytesPerSec;
      if (e.bytesPerSecMax > 0) e.remainingSecLow = usable / e.bytesPerSecMax;
      if (e.bytesPerSecMin > 0) e.remainingSecHigh = usable / e.bytesPerSecMin;
      e.warning = freeB < kDiskWarnBytes || (e.remainingSecLow >= 0 && e.remainingSecLow < nsToSec(kDiskWarnRemainingNs)) || (e.remainingSec >= 0 && e.remainingSec < nsToSec(kDiskWarnRemainingNs));
      char b[200]; std::snprintf(b, sizeof b, "%s free; about %.0f min left (range %.0f-%.0f min) at %s/s%s", humanBytes((double)freeB).c_str(), e.remainingSec / 60, e.remainingSecLow / 60, e.remainingSecHigh / 60, humanBytes(e.bytesPerSec).c_str(), e.rateMeasured ? "" : " (estimated, not yet measured)");
      e.text = b;
    } else e.text = "free space unknown";
    { std::lock_guard<std::mutex> l(m); disk = e; }
    if (e.warning) addWarning("Disk space is low: " + e.text);
    if (e.known && e.freeBytes <= e.reserveBytes && !dead.load())
      failGraceful(Status::Fail(Err::DiskFull, "Free disk space (" + humanBytes((double)e.freeBytes) + ") reached the graceful-stop reserve (" + humanBytes((double)e.reserveBytes) + "); recording was stopped and all files finalized. Free space and start a new recording.", true));
  }

  // ------------------------------------------------------------------ pump / thread
  bool queuesEmpty() { std::lock_guard<std::mutex> l(qm); return an == 0 && vn == 0; }
  bool pump() {
    bool did = false;
    for (;;) {
      AudioBlockPtr b;
      { std::lock_guard<std::mutex> l(qm); if (!an) break; b = aring[ah]; aring[ah].reset(); ah = (ah + 1) % aring.size(); --an; aframes -= b->frames(); }
      did = true;
      if (dead.load()) { lostLate += b->frames(); aFramesDropped += 0; continue; }
      processAudio(*b);
    }
    for (;;) {
      std::shared_ptr<const Frame> f;
      { std::lock_guard<std::mutex> l(qm); if (!vn) break; f = vring[vh]; vring[vh].reset(); vh = (vh + 1) % 3; --vn; }
      did = true;
      if (dead.load()) continue;
      processVideo(*f);
    }
    if (!dead.load() && !finalized) {
      Ns now = d.clock->nowNs();
      if (now >= nextDiskPoll) { nextDiskPoll = now + 250 * kNsPerMs; diskPoll(now); refreshSegments(); }
      if (metaDirty.exchange(false)) sidecarBestEffort();
    }
    if (haveLost) snapshotLost();
    return did;
  }
  void finalizeNormal() {
    if (finalized || dead.load()) return;
    finalized = true; accepting = false;
    Status c = closeWriters();
    RecState ns = c.ok() ? RecState::Completed : RecState::Recoverable;
    { std::lock_guard<std::mutex> l(m); if (!c.ok()) err = c; st = ns; }
    uint64_t freeB = 0; (void)freeB;
    sidecarBestEffort(); setState(ns);
  }
  void run() {
    for (;;) {
      bool did = pump();
      bool done = (stopReq.load() || dead.load()) && queuesEmpty();
      if (done) break;
      if (!did) { std::unique_lock<std::mutex> l(qm); qcv.wait_for(l, std::chrono::milliseconds(20), [&] { return an || vn || stopReq.load(); }); }
    }
    pump();
    if (!dead.load()) finalizeNormal();
  }
};

// =============================================================================== public
RecordingSession::RecordingSession(const SessionDeps& d) : p_(new Impl) { p_->d = d; p_->aring.assign(kAudioQueueMaxBlocks, AudioBlockPtr()); }
RecordingSession::~RecordingSession() {
  RecState s = state();
  if (s == RecState::Recording || s == RecState::Stopping) stop();
  if (p_->th.joinable()) p_->th.join();
}
void RecordingSession::setStateCallback(std::function<void(RecState)> cb) { std::lock_guard<std::mutex> l(p_->m); p_->cb = std::move(cb); }
RecState RecordingSession::state() const { std::lock_guard<std::mutex> l(p_->m); return p_->st; }
const RecordingProfile& RecordingSession::profile() const { return p_->prof; }

Status RecordingSession::start(const RecordingRequest& r) {
  {
    std::lock_guard<std::mutex> l(p_->m);
    if (p_->st == RecState::Preparing || p_->st == RecState::Recording || p_->st == RecState::Stopping)
      return Status::Fail(Err::InvalidArgument, "A recording session is already active (" + std::string(recStateName(p_->st)) + "); stop it first. The active profile cannot be changed.");
  }
  if (p_->th.joinable()) p_->th.join();
  p_->resetRun();
  p_->setState(RecState::Preparing);
  Status s = p_->prepare(r);
  if (!s.ok()) {
    if (p_->wavw) { p_->wavw->close(); }
    if (p_->mw) { p_->mw->close(); }
    p_->journal.close();
    { std::lock_guard<std::mutex> l(p_->m); p_->err = s; }
    p_->setState(RecState::Failed);
    return s;
  }
  p_->accepting = true;
  p_->setState(RecState::Recording);
  if (!p_->d.manualPump) p_->th = std::thread([this] { p_->run(); });
  return Status::Ok();
}

bool RecordingSession::pushAudio(const AudioBlockPtr& b) {
  if (!b) return false;
  size_t frames = b->frames();
  if (!p_->accepting.load(std::memory_order_acquire)) { if (p_->dead.load()) p_->lostLate += frames; return false; }
  {
    std::lock_guard<std::mutex> l(p_->qm);
    if (p_->an >= p_->aring.size() || p_->aframes + frames > (uint64_t)p_->prof.audioRate * 2) {
      ++p_->aBlocksDropped; p_->aFramesDropped += frames; return false;
    }
    p_->aring[(p_->ah + p_->an) % p_->aring.size()] = b; ++p_->an; p_->aframes += frames; ++p_->inputBlocks;
  }
  p_->qcv.notify_one();
  return true;
}
bool RecordingSession::pushVideo(const std::shared_ptr<const Frame>& f) {
  if (!f || !p_->av || !p_->accepting.load(std::memory_order_acquire)) return false;
  {
    std::lock_guard<std::mutex> l(p_->qm);
    if (p_->vn == 3) { p_->vring[p_->vh].reset(); p_->vh = (p_->vh + 1) % 3; --p_->vn; ++p_->vQueueDropped; }   // newest frames win
    p_->vring[(p_->vh + p_->vn) % 3] = f; ++p_->vn;
  }
  p_->qcv.notify_one();
  return true;
}
Status RecordingSession::addMarker(const std::string& label) {
  RecState s = state();
  if (s != RecState::Recording) return Status::Fail(Err::InvalidArgument, "Markers can only be added while recording.");
  Marker mk; mk.label = label.substr(0, 200);
  for (auto& c : mk.label) if ((unsigned char)c < 0x20) c = ' ';
  mk.timelineNs = p_->anchoredA.load() ? std::max<Ns>(0, p_->d.clock->nowNs() - p_->originNs.load()) : 0;
  { std::lock_guard<std::mutex> l(p_->mm);
    if (p_->markers.size() >= 10000) return Status::Fail(Err::ResourceExhausted, "Marker limit (10000) reached for this session.");
    mk.id = p_->nextMarker++; p_->markers.push_back(mk); }
  p_->metaDirty = true; p_->qcv.notify_one();
  return Status::Ok();
}
void RecordingSession::noteShowEvent(const std::string& what) {
  std::lock_guard<std::mutex> l(p_->m);
  if (p_->showEvents.size() < 1000) p_->showEvents.push_back(what.substr(0, 200));   // metadata only; no clock interaction by design
  p_->metaDirty = true;
}
void RecordingSession::noteBlockTelemetry(bool nfs, bool clip, bool lim, float gr) {
  if (nfs) ++p_->nearFS;
  if (clip) ++p_->clipB;
  if (lim) ++p_->limB;
  if (gr > 0) { uint32_t mg = (uint32_t)(gr * 1000.f); uint32_t cur = p_->maxGrMilli.load(); while (mg > cur && !p_->maxGrMilli.compare_exchange_weak(cur, mg)) {} }
}
void RecordingSession::setDynamics(bool bypassed) {
  std::lock_guard<std::mutex> l(p_->m);
  Ns t = p_->anchoredA.load() ? std::max<Ns>(0, p_->d.clock->nowNs() - p_->originNs.load()) : 0;
  p_->dynBypassed = bypassed; p_->dynEvents.push_back(std::string(bypassed ? "bypassed" : "enabled") + " @" + std::to_string(t / kNsPerMs) + "ms");
  if (bypassed) { bool have = false; for (auto& w : p_->warnings) if (w.find("bypassed") != std::string::npos) have = true; if (!have) p_->warnings.push_back("Dynamics (compressor/limiter) are bypassed: the recording is unprocessed and may clip."); }
  p_->metaDirty = true;
}
bool RecordingSession::pump() { return p_->d.manualPump ? p_->pump() : false; }

Status RecordingSession::stop() {
  RecState s = state();
  if (s == RecState::Idle || s == RecState::Completed) return Status::Ok();
  if (s == RecState::Failed || s == RecState::Recoverable) { if (p_->th.joinable()) p_->th.join(); std::lock_guard<std::mutex> l(p_->m); return p_->err; }
  if (s == RecState::Preparing) return Status::Fail(Err::InvalidArgument, "Session is still preparing.");
  if (s == RecState::Recording) p_->setState(RecState::Stopping);
  p_->accepting = false; p_->stopReq = true; p_->qcv.notify_all();
  if (p_->th.joinable()) p_->th.join();
  else { p_->pump(); p_->finalizeNormal(); }
  std::lock_guard<std::mutex> l(p_->m);
  return p_->st == RecState::Completed ? Status::Ok() : p_->err;
}

RecordingStatus RecordingSession::status() const {
  RecordingStatus r;
  { std::lock_guard<std::mutex> l(p_->m);
    r.state = p_->st; r.error = p_->err; r.warnings = p_->warnings; r.disk = p_->disk; r.gaps = p_->gaps; r.lost = p_->lost; r.segments = p_->segSnap; }
  if (!r.lost.empty()) {   // keep the open-ended lost interval current while audio keeps arriving
    uint64_t extra = 0; { std::lock_guard<std::mutex> l(p_->m); extra = p_->lostLate.load() - p_->lostLateAtSnap; }
    LostInterval& li = r.lost[0]; li.frames += extra; li.toNs += samplesToNs(extra, p_->prof.audioRate);
  }
  r.audioFramesWritten = p_->outFrames; r.audioBlocksDropped = p_->aBlocksDropped; r.audioFramesDropped = p_->aFramesDropped;
  r.videoFramesQueueDropped = p_->vQueueDropped; r.gapCount = p_->gapCount; r.gapFrames = p_->gapFrames; r.bytesWritten = p_->bytesNow; r.inputBlocks = p_->inputBlocks;
  r.nearFullScaleBlocks = p_->nearFS; r.possibleClippingBlocks = p_->clipB; r.limiterActiveBlocks = p_->limB; r.maxGainReductionDb = p_->maxGrMilli.load() / 1000.f;
  { std::lock_guard<std::mutex> l(p_->qm); r.queuedAudioFrames = p_->aframes; }
  { std::lock_guard<std::mutex> l(p_->mm); r.markers = p_->markers.size(); }
  { std::lock_guard<std::mutex> l(p_->m); r.videoDropped = p_->vDropSnap; r.videoDuplicated = p_->vDupSnap; r.drift = p_->driftSnap; }
  r.timelineNs = samplesToNs(r.audioFramesWritten, p_->prof.audioRate);
  r.journalPath = p_->journalPath; r.metadataPath = p_->metaPath;
  return r;
}

std::vector<RecoveryItem> recoverSession(IFileSystem& fs, const std::string& journalPath) {
  std::vector<RecoveryItem> out; Bytes jb;
  if (!fs.readAll(journalPath, jb)) { RecoveryItem it; it.path = journalPath; it.kind = "journal"; it.detail = "journal not found"; out.push_back(it); return out; }
  for (auto& e : SegmentJournal::parse(jb)) {
    RecoveryItem it; it.path = e.seg.path; it.kind = e.seg.kind;
    if (!fs.exists(e.seg.path)) { it.detail = "segment file missing"; out.push_back(it); continue; }
    if (!e.active) { it.ok = true; it.detail = "completed before stop (not touched)"; out.push_back(it); continue; }
    if (e.seg.kind == "wav") { WavRecovery r = recoverWav(fs, e.seg.path); it.ok = r.ok; it.repaired = r.repaired; it.detail = r.detail + " (" + std::to_string(r.frames) + " frames)"; }
    else if (e.seg.kind == "avi") { AviRecovery r = recoverAvi(fs, e.seg.path); it.ok = r.ok; it.repaired = r.repaired; it.detail = r.detail + " (" + std::to_string(r.videoSlots) + " video slots, " + std::to_string(r.audioFrames) + " audio frames)"; }
    else { it.ok = true; it.detail = "Matroska written streaming-style (unknown-size Segment/Cluster): readable up to the last complete block; no Cues until finalized"; }
    out.push_back(it);
  }
  return out;
}

} // namespace ss
