#include "recording/video_writer.h"
#include "recording/naming.h"
#include <algorithm>
#include <cstring>
namespace ss {
using namespace rec;

const char* containerExt(MediaContainer c) { return c == MediaContainer::Avi ? "avi" : "mkv"; }

// =============================== MediaWriter base ===============================
MediaWriter::~MediaWriter() {}   // derived destructors call close() (no virtual calls from here)

Status MediaWriter::open(const MediaWriterConfig& c) {
  if (opened_) return Status::Fail(Err::InvalidArgument, "writer already open");
  if (c.codec != VideoCodec::MJPEG) return Status::Fail(Err::UnsupportedCapability, std::string("container writer cannot store ") + codecName(c.codec) + "; only MJPEG is writable in this build");
  if (c.width < 16 || c.height < 16 || c.fpsNum <= 0 || c.fpsDen <= 0) return Status::Fail(Err::InvalidArgument, "invalid video geometry/frame rate");
  if (c.hasAudio && (c.audioChannels < 1 || c.audioChannels > 2 || (c.audioBits != 16 && c.audioBits != 24) || c.audioRate < 8000))
    return Status::Fail(Err::InvalidArgument, "unsupported audio format (1-2 channels, 16/24-bit)");
  cfg_ = c;
  uint64_t hard = hardLimitBytes();
  cfg_.maxSegmentBytes = std::min<uint64_t>(std::max<uint64_t>(cfg_.maxSegmentBytes, 4096), hard);
  audioCapFrames_ = std::max<uint64_t>(1, nsToSamples(cfg_.maxSegmentNs, cfg_.audioRate));
  cad_.configure(cfg_.fpsNum, cfg_.fpsDen);
  nextSample_ = cfg_.firstSample; opened_ = true;
  if (cfg_.hasAudio) { Status s = startSegment(0); if (!s.ok()) return fail(s); }
  return Status::Ok();
}

Status MediaWriter::fail(const Status& s) {
  if (!failed_) { failed_ = true; err_ = s; }
  if (inSeg_) {   // leave the active segment unfinalized (journal keeps it "active") for recoverAvi/stream readers
    if (sink_) { sink_->flush(); sink_->close(); sink_.reset(); }
    if (!segs_.empty()) { segs_.back().complete = false; segs_.back().audioFrames = segAudio_; }
    if (j_) j_->note("write_error", s.detail);
    inSeg_ = false;
  }
  return err_;
}

Status MediaWriter::startSegment(Ns hint) {
  SegmentInfo s; s.index = (int)segs_.size(); s.kind = ext();
  s.path = joinPath(cfg_.dir, segmentName(cfg_.base, s.index, ext()));
  s.firstSample = nextSample_;
  segStartNs_ = cfg_.hasAudio ? samplesToNs(nextSample_, cfg_.audioRate) : hint;
  s.startNs = segStartNs_; s.endNs = segStartNs_;
  Status st = fs_.open(s.path, IFileSystem::Mode::Truncate, sink_);
  if (!st.ok()) return st;
  segAudio_ = 0; segVideoSlots_ = 0; cad_.reset(0);
  st = beginSegment(*sink_, s);
  if (!st.ok()) return st;
  segs_.push_back(s); inSeg_ = true;
  if (j_) { Status js = j_->segmentOpen(s); if (!js.ok()) return js; }
  return Status::Ok();
}

Status MediaWriter::finishSegment(bool clean) {
  if (!inSeg_) return Status::Ok();
  SegmentInfo& s = segs_.back();
  s.audioFrames = segAudio_;
  s.endNs = segStartNs_ + (cfg_.hasAudio ? samplesToNs(segAudio_, cfg_.audioRate) : 0);
  Status e = endSegment(s);
  Status c = sink_->flush();
  s.bytes = sink_->size();
  sink_->close(); sink_.reset(); inSeg_ = false;
  s.complete = clean && e.ok() && c.ok();
  if (j_) j_->segmentClose(s);
  if (s.complete) durableAudio_ = nextSample_ - cfg_.firstSample;
  return e.ok() ? c : e;
}

Status MediaWriter::writeAudio(const float* in, size_t frames) {
  if (!opened_ || closed_) return Status::Fail(Err::InvalidArgument, "writer not open");
  if (failed_) return err_;
  if (!cfg_.hasAudio) return Status::Fail(Err::InvalidArgument, "writer configured without audio");
  const uint32_t ch = cfg_.audioChannels; const uint32_t bps = cfg_.audioBits / 8; const uint64_t bpf = bytesPerFrame();
  while (frames) {
    if (!inSeg_) { Status s = startSegment(0); if (!s.ok()) return fail(s); }
    if (segAudio_ >= audioCapFrames_) { Status s = finishSegment(true); if (!s.ok()) return fail(s); continue; }
    uint64_t proj = projectedBytes(0);
    uint64_t room = proj < cfg_.maxSegmentBytes ? cfg_.maxSegmentBytes - proj : 0;
    uint64_t fit = room / bpf; fit = fit > 8 ? fit - 8 : 0;
    if (fit == 0) {
      if (segAudio_ == 0 && segVideoSlots_ == 0) return fail(Status::Fail(Err::ResourceExhausted, "segment size budget too small for a single audio chunk", false));
      Status s = finishSegment(true); if (!s.ok()) return fail(s); continue;
    }
    size_t take = (size_t)std::min<uint64_t>(std::min<uint64_t>(frames, audioCapFrames_ - segAudio_), fit);
    scratch_.resize(take * bpf);
    for (size_t i = 0; i < take * ch; ++i) {
      bool c = bps == 3 ? putPcm24(scratch_.data() + i * 3, in[i]) : putPcm16(scratch_.data() + i * 2, in[i]);
      if (c) ++clamped_;
    }
    Status s = putAudio(scratch_.data(), take, segAudio_);
    if (!s.ok()) return fail(s);
    segAudio_ += take; nextSample_ += take; frames -= take; in += take * ch;
  }
  return Status::Ok();
}

Status MediaWriter::writeVideo(Ns pts, const uint8_t* d, size_t n, bool key) {
  if (!opened_ || closed_) return Status::Fail(Err::InvalidArgument, "writer not open");
  if (failed_) return err_;
  if (!inSeg_) { Status s = startSegment(pts); if (!s.ok()) return fail(s); }
  // roll decisions only on key frames
  if (key && (segVideoSlots_ > 0 || segAudio_ > 0)) {
    bool timeUp = (pts - segStartNs_) >= cfg_.maxSegmentNs;
    bool sizeUp = projectedBytes(n + 64) > cfg_.maxSegmentBytes;
    if (timeUp || sizeUp) {
      Status s = finishSegment(true); if (!s.ok()) return fail(s);
      s = startSegment(pts); if (!s.ok()) return fail(s);
    }
  }
  if (projectedBytes(n + 64) > hardLimitBytes()) return fail(Status::Fail(Err::ResourceExhausted, "video frame does not fit the segment size limit", false));
  Ns rel = pts - segStartNs_;
  if (rel < 0) { rel = 0; ++earlyVideo_; }
  int64_t slot = 0; uint32_t dup = 0;
  if (fixedFps()) {
    FrameCadence::Slot sl = cad_.push(rel);
    if (sl.drop) { ++droppedVideo_; return Status::Ok(); }
    slot = sl.slot; dup = sl.dupBefore; dupVideo_ += dup;
  }
  Status s = putVideo(rel, slot, dup, d, n, key);
  if (!s.ok()) return fail(s);
  segVideoSlots_ += 1 + dup;
  segs_.back().videoFrames = segVideoSlots_; segs_.back().videoDups += dup;
  return Status::Ok();
}

Status MediaWriter::close() {
  if (closed_) return failed_ ? err_ : Status::Ok();
  closed_ = true;
  if (!opened_ || failed_) return failed_ ? err_ : Status::Ok();
  Status s = finishSegment(true);
  if (!s.ok()) { failed_ = true; err_ = s; }
  return s;
}

// =============================== AVI ===============================
namespace {
const uint64_t kAviHard = (2ull << 30) - (64ull << 20);   // < 2 GiB, with margin

class AviWriter : public MediaWriter {
 public:
  AviWriter(IFileSystem& fs, SegmentJournal* j) : MediaWriter(fs, j) {}
  ~AviWriter() override { close(); }
 protected:
  const char* ext() const override { return "avi"; }
  bool fixedFps() const override { return true; }
  uint64_t hardLimitBytes() const override { return kAviHard; }

  static size_t beginList(Bytes& h, const char* type) { putTag(h, "LIST"); size_t at = h.size(); put32(h, 0); putTag(h, type); return at; }
  static void endList(Bytes& h, size_t at) { patch32(h, at, (uint32_t)(h.size() - at - 4)); }

  Status beginSegment(IFileSink& sink, const SegmentInfo&) override {
    Bytes h; const bool A = cfg_.hasAudio;
    putTag(h, "RIFF"); put32(h, 0); putTag(h, "AVI ");
    size_t hdrl = beginList(h, "hdrl");
    putTag(h, "avih"); put32(h, 56);
    put32(h, (uint32_t)((1000000.0 * cfg_.fpsDen) / cfg_.fpsNum + 0.5));
    put32(h, 0); put32(h, 0); put32(h, 0x110);
    avihTotalAt_ = h.size(); put32(h, 0);
    put32(h, 0); put32(h, A ? 2 : 1); put32(h, 1 << 20); put32(h, cfg_.width); put32(h, cfg_.height);
    for (int i = 0; i < 4; ++i) put32(h, 0);
    size_t sv = beginList(h, "strl");
    putTag(h, "strh"); put32(h, 56);
    putTag(h, "vids"); putTag(h, "MJPG"); put32(h, 0); put16(h, 0); put16(h, 0); put32(h, 0);
    put32(h, cfg_.fpsDen); put32(h, cfg_.fpsNum);
    vStartAt_ = h.size(); put32(h, 0); vLenAt_ = h.size(); put32(h, 0);
    put32(h, 1 << 20); put32(h, 0xFFFFFFFFu); put32(h, 0);
    put16(h, 0); put16(h, 0); put16(h, (uint32_t)cfg_.width); put16(h, (uint32_t)cfg_.height);
    putTag(h, "strf"); put32(h, 40);
    put32(h, 40); put32(h, cfg_.width); put32(h, cfg_.height); put16(h, 1); put16(h, 24); putTag(h, "MJPG");
    put32(h, (uint32_t)cfg_.width * cfg_.height * 3); put32(h, 0); put32(h, 0); put32(h, 0); put32(h, 0);
    endList(h, sv);
    if (A) {
      uint32_t ba = (uint32_t)bytesPerFrame();
      size_t sa = beginList(h, "strl");
      putTag(h, "strh"); put32(h, 56);
      putTag(h, "auds"); put32(h, 0); put32(h, 0); put16(h, 0); put16(h, 0); put32(h, 0);
      put32(h, ba); put32(h, cfg_.audioRate * ba);
      put32(h, 0); aLenAt_ = h.size(); put32(h, 0);
      put32(h, 1 << 16); put32(h, 0xFFFFFFFFu); put32(h, ba);
      put16(h, 0); put16(h, 0); put16(h, 0); put16(h, 0);
      putTag(h, "strf"); put32(h, 16);
      put16(h, 1); put16(h, cfg_.audioChannels); put32(h, cfg_.audioRate); put32(h, cfg_.audioRate * ba); put16(h, ba); put16(h, cfg_.audioBits);
      endList(h, sa);
    }
    endList(h, hdrl);
    putTag(h, "LIST"); moviSizeAt_ = h.size(); put32(h, 0); moviTag_ = h.size(); putTag(h, "movi");
    idx_.clear(); pendA_.clear(); pendFrames_ = 0; nChunks_ = 0; vChunks_ = 0; vDups_ = 0; aFrames_ = 0; firstSlot_ = 0; haveFirst_ = false;
    return sink.write(h.data(), h.size());
  }

  Status chunk(const char* id, const uint8_t* d, size_t n, uint32_t flags) {
    uint64_t off = sink_->size();
    buf_.clear(); putTag(buf_, id); put32(buf_, (uint32_t)n);
    if (n) putBytes(buf_, d, n);
    if (n & 1) buf_.push_back(0);
    Status s = sink_->write(buf_.data(), buf_.size());
    if (!s.ok()) return s;
    putTag(idx_, id); put32(idx_, flags); put32(idx_, (uint32_t)(off - moviTag_)); put32(idx_, (uint32_t)n);
    ++nChunks_; return Status::Ok();
  }
  Status flushAudio() {
    if (!pendFrames_) return Status::Ok();
    Status s = chunk("01wb", pendA_.data(), pendA_.size(), 0x10);
    aFrames_ += pendFrames_; pendA_.clear(); pendFrames_ = 0; return s;
  }
  Status putAudio(const uint8_t* pcm, size_t frames, uint64_t) override {
    putBytes(pendA_, pcm, frames * bytesPerFrame()); pendFrames_ += frames;
    uint64_t perVideo = std::max<uint64_t>(1, (uint64_t)cfg_.audioRate * cfg_.fpsDen / cfg_.fpsNum);
    if (pendFrames_ >= perVideo) return flushAudio();
    return Status::Ok();
  }
  Status putVideo(Ns, int64_t slot, uint32_t dupBefore, const uint8_t* d, size_t n, bool key) override {
    Status s = flushAudio(); if (!s.ok()) return s;
    if (!haveFirst_) { haveFirst_ = true; firstSlot_ = (uint32_t)slot; dupBefore = 0; }
    for (uint32_t i = 0; i < dupBefore; ++i) { s = chunk("00dc", nullptr, 0, 0); if (!s.ok()) return s; ++vChunks_; ++vDups_; }
    s = chunk("00dc", d, n, key ? 0x10 : 0); if (!s.ok()) return s;
    ++vChunks_; return Status::Ok();
  }
  uint64_t projectedBytes(uint64_t add) const override {
    return (sink_ ? sink_->size() : 0) + pendA_.size() + add + 16 * (nChunks_ + 4) + 8 + 8192;
  }
  Status endSegment(SegmentInfo& seg) override {
    Status s = flushAudio(); if (!s.ok()) return s;
    uint64_t idxPos = sink_->size();
    Bytes ih; putTag(ih, "idx1"); put32(ih, (uint32_t)idx_.size());
    s = sink_->write(ih.data(), ih.size()); if (!s.ok()) return s;
    if (!idx_.empty()) { s = sink_->write(idx_.data(), idx_.size()); if (!s.ok()) return s; }
    uint64_t total = sink_->size();
    auto p32 = [&](uint64_t at, uint32_t v) { Bytes b; put32(b, v); return sink_->writeAt(at, b.data(), 4); };
    if (!(s = p32(4, (uint32_t)(total - 8))).ok()) return s;
    if (!(s = p32(moviSizeAt_, (uint32_t)(idxPos - moviTag_))).ok()) return s;
    if (!(s = p32(avihTotalAt_, vChunks_)).ok()) return s;
    if (!(s = p32(vStartAt_, firstSlot_)).ok()) return s;
    if (!(s = p32(vLenAt_, vChunks_)).ok()) return s;
    if (cfg_.hasAudio && !(s = p32(aLenAt_, (uint32_t)aFrames_)).ok()) return s;
    seg.videoFrames = vChunks_; seg.videoDups = vDups_;
    return Status::Ok();
  }
 private:
  uint64_t avihTotalAt_ = 0, vStartAt_ = 0, vLenAt_ = 0, aLenAt_ = 0, moviSizeAt_ = 0, moviTag_ = 0;
  Bytes idx_, pendA_, buf_; uint64_t pendFrames_ = 0, nChunks_ = 0, vChunks_ = 0, vDups_ = 0, aFrames_ = 0; uint32_t firstSlot_ = 0; bool haveFirst_ = false;
};

// =============================== MKV ===============================
void eId(Bytes& b, uint32_t id) { if (id > 0xFFFFFF) put8(b, id >> 24); if (id > 0xFFFF) put8(b, id >> 16); if (id > 0xFF) put8(b, id >> 8); put8(b, id); }
void eSize(Bytes& b, uint64_t n) {
  int len = 1; while (len < 8 && n >= ((1ull << (7 * len)) - 1)) ++len;
  put8(b, (uint32_t)((1u << (8 - len)) | (n >> (8 * (len - 1)))));
  for (int i = len - 2; i >= 0; --i) put8(b, (uint32_t)(n >> (8 * i)));
}
void eSize8(Bytes& b, uint64_t n) { put8(b, 0x01); for (int i = 6; i >= 0; --i) put8(b, (uint32_t)(n >> (8 * i))); }
void eUnknown8(Bytes& b) { put8(b, 0x01); for (int i = 0; i < 7; ++i) put8(b, 0xFF); }
Bytes eElem(uint32_t id, const Bytes& payload) { Bytes b; eId(b, id); eSize(b, payload.size()); b.insert(b.end(), payload.begin(), payload.end()); return b; }
Bytes eUint(uint32_t id, uint64_t v) { Bytes p; int n = 1; while (n < 8 && (v >> (8 * n))) ++n; for (int i = n - 1; i >= 0; --i) put8(p, (uint32_t)(v >> (8 * i))); return eElem(id, p); }
Bytes eStr(uint32_t id, const std::string& s) { Bytes p(s.begin(), s.end()); return eElem(id, p); }
void beF64(Bytes& b, double d) { uint64_t u; std::memcpy(&u, &d, 8); for (int i = 7; i >= 0; --i) put8(b, (uint32_t)(u >> (8 * i))); }
Bytes eF64(uint32_t id, double d) { Bytes p; beF64(p, d); return eElem(id, p); }
void cat(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }

class MkvWriter : public MediaWriter {
 public:
  MkvWriter(IFileSystem& fs, SegmentJournal* j) : MediaWriter(fs, j) {}
  ~MkvWriter() override { close(); }
 protected:
  const char* ext() const override { return "mkv"; }
  bool fixedFps() const override { return false; }
  uint64_t hardLimitBytes() const override { return 3ull << 30; }
  uint32_t vTrack() const { return 1; }
  uint32_t aTrack() const { return 2; }

  Status beginSegment(IFileSink& sink, const SegmentInfo&) override {
    Bytes out;
    { Bytes e; cat(e, eUint(0x4286, 1)); cat(e, eUint(0x42F7, 1)); cat(e, eUint(0x42F2, 4)); cat(e, eUint(0x42F3, 8));
      cat(e, eStr(0x4282, "matroska")); cat(e, eUint(0x4287, 4)); cat(e, eUint(0x4285, 2)); cat(out, eElem(0x1A45DFA3, e)); }
    eId(out, 0x18538067); segSizeAt_ = out.size(); eUnknown8(out); segData_ = out.size();
    // Info
    Bytes info; cat(info, eUint(0x2AD7B1, 1000000)); cat(info, eStr(0x4D80, "StageSignal-mux")); cat(info, eStr(0x5741, cfg_.writingApp));
    { Bytes d; beF64(d, 0.0); Bytes el = eElem(0x4489, d); durOffInInfo_ = info.size() + el.size() - 8; cat(info, el); }
    Bytes infoEl = eElem(0x1549A966, info);
    durOffInInfo_ += infoEl.size() - info.size();
    // Tracks
    Bytes tracks;
    { Bytes t; cat(t, eUint(0xD7, vTrack())); cat(t, eUint(0x73C5, 1)); cat(t, eUint(0x83, 1)); cat(t, eUint(0x9C, 0));
      cat(t, eStr(0x86, "V_MJPEG")); cat(t, eUint(0x23E383, (uint64_t)(1e9 * cfg_.fpsDen / cfg_.fpsNum + 0.5)));
      Bytes v; cat(v, eUint(0xB0, (uint64_t)cfg_.width)); cat(v, eUint(0xBA, (uint64_t)cfg_.height)); cat(t, eElem(0xE0, v));
      cat(tracks, eElem(0xAE, t)); }
    if (cfg_.hasAudio) {
      Bytes t; cat(t, eUint(0xD7, aTrack())); cat(t, eUint(0x73C5, 2)); cat(t, eUint(0x83, 2)); cat(t, eUint(0x9C, 0));
      cat(t, eStr(0x86, "A_PCM/INT/LIT"));
      Bytes a; cat(a, eF64(0xB5, (double)cfg_.audioRate)); cat(a, eUint(0x9F, cfg_.audioChannels)); cat(a, eUint(0x6264, cfg_.audioBits)); cat(t, eElem(0xE1, a));
      cat(tracks, eElem(0xAE, t));
    }
    Bytes tracksEl = eElem(0x1654AE6B, tracks);
    // SeekHead with fixed-width positions (Cues/Tags patched at finalize)
    auto seek = [&](uint32_t id, uint64_t pos, uint64_t& fieldOff, uint64_t base) {
      Bytes s; Bytes idb; eId(idb, id); cat(s, eElem(0x53AB, idb));
      Bytes pf; eId(pf, 0x53AC); eSize(pf, 8); size_t fo = pf.size(); for (int i = 7; i >= 0; --i) put8(pf, (uint32_t)(pos >> (8 * i)));
      fieldOff = base + s.size() + (fo); cat(s, pf);
      Bytes el; eId(el, 0x4DBB); eSize(el, s.size()); size_t hdr = el.size(); cat(el, s); fieldOff += hdr; return el; };
    // compute sizes with dummy positions
    uint64_t d1, d2, d3, d4;
    size_t shSize; { Bytes t; cat(t, seek(0x1549A966, 0, d1, 0)); cat(t, seek(0x1654AE6B, 0, d2, 0)); cat(t, seek(0x1254C367, 0, d3, 0)); cat(t, seek(0x1C53BB6B, 0, d4, 0)); shSize = eElem(0x114D9B74, t).size(); }
    uint64_t infoPos = shSize, tracksPos = shSize + infoEl.size();
    Bytes sh; uint64_t fo3 = 0, fo4 = 0, dummy;
    { Bytes t; Bytes a = seek(0x1549A966, infoPos, dummy, 0); cat(t, a);
      Bytes b = seek(0x1654AE6B, tracksPos, dummy, t.size()); cat(t, b);
      Bytes c = seek(0x1254C367, 0, fo3, t.size()); cat(t, c);
      Bytes d = seek(0x1C53BB6B, 0, fo4, t.size()); cat(t, d);
      sh = eElem(0x114D9B74, t);
      size_t hdr = sh.size() - t.size(); fo3 += hdr; fo4 += hdr; }
    tagsSeekAt_ = out.size() + fo3; cuesSeekAt_ = out.size() + fo4;
    cat(out, sh);
    durAt_ = out.size() + durOffInInfo_; cat(out, infoEl); cat(out, tracksEl);
    clOpen_ = false; cues_.clear(); pendA_.clear(); pendFrames_ = 0; maxMs_ = 0; nClusters_ = 0; videoCount_ = 0;
    return sink.write(out.data(), out.size());
  }
  int64_t msOfSample(uint64_t s) const { return (int64_t)((s * 1000ull + cfg_.audioRate / 2) / cfg_.audioRate); }
  Status closeCluster() {
    if (!clOpen_) return Status::Ok();
    clOpen_ = false;
    Bytes b; eSize8(b, clBytes_);
    cues_.push_back({clMs_, clPos_ - segData_});
    return sink_->writeAt(clSizeAt_, b.data(), 8);
  }
  Status openCluster(int64_t ms) {
    Bytes b; clPos_ = sink_->size(); eId(b, 0x1F43B675); clSizeAt_ = clPos_ + b.size(); eUnknown8(b);
    Bytes tc = eUint(0xE7, (uint64_t)ms); cat(b, tc);
    clBytes_ = tc.size(); clMs_ = ms; clOpen_ = true; ++nClusters_;
    return sink_->write(b.data(), b.size());
  }
  Status block(uint32_t track, int64_t ms, bool key, const uint8_t* d, size_t n) {
    if (ms < 0) ms = 0;
    if (!clOpen_ || ms - clMs_ >= 5000) { Status s = closeCluster(); if (!s.ok()) return s; s = openCluster(ms); if (!s.ok()) return s; }
    int64_t rel = ms - clMs_;
    Bytes b; eId(b, 0xA3); eSize(b, n + 4);
    put8(b, 0x80 | track); put8(b, (uint32_t)((rel >> 8) & 255)); put8(b, (uint32_t)(rel & 255)); put8(b, key ? 0x80 : 0);
    putBytes(b, d, n);
    Status s = sink_->write(b.data(), b.size());
    if (s.ok()) { clBytes_ += b.size(); if (ms > maxMs_) maxMs_ = ms; }
    return s;
  }
  Status flushAudio() {
    if (!pendFrames_) return Status::Ok();
    Status s = block(aTrack(), msOfSample(pendStart_), true, pendA_.data(), pendA_.size());
    pendA_.clear(); pendFrames_ = 0; return s;
  }
  Status putAudio(const uint8_t* pcm, size_t frames, uint64_t segFrame) override {
    if (!pendFrames_) pendStart_ = segFrame;
    putBytes(pendA_, pcm, frames * bytesPerFrame()); pendFrames_ += frames;
    if (pendFrames_ >= cfg_.audioRate / 50) return flushAudio();
    return Status::Ok();
  }
  Status putVideo(Ns rel, int64_t, uint32_t, const uint8_t* d, size_t n, bool key) override {
    ++videoCount_;
    return block(vTrack(), (rel + 500000) / 1000000, key, d, n);
  }
  uint64_t projectedBytes(uint64_t add) const override {
    return (sink_ ? sink_->size() : 0) + pendA_.size() + add + 32 + 32 * (nClusters_ + 2) + 16384;
  }
  Status endSegment(SegmentInfo& seg) override {
    Status s = flushAudio(); if (!s.ok()) return s;
    s = closeCluster(); if (!s.ok()) return s;
    // Tags
    uint64_t tagsPos = sink_->size();
    Bytes tags;
    { Tags kv; kv.emplace_back("STAGESIGNAL_SEGMENT", std::to_string(seg.index));
      kv.emplace_back("STAGESIGNAL_FIRST_SAMPLE", std::to_string(seg.firstSample));
      kv.emplace_back("STAGESIGNAL_START_NS", std::to_string(seg.startNs));
      if (tags_) { Tags more = tags_(seg); kv.insert(kv.end(), more.begin(), more.end()); }
      Bytes tag; cat(tag, eElem(0x63C0, Bytes()));
      for (auto& p : kv) { Bytes st; cat(st, eStr(0x45A3, p.first)); cat(st, eStr(0x4487, p.second)); cat(tag, eElem(0x67C8, st)); }
      cat(tags, eElem(0x1254C367, eElem(0x7373, tag))); }
    s = sink_->write(tags.data(), tags.size()); if (!s.ok()) return s;
    uint64_t cuesPos = sink_->size();
    Bytes cues;
    { Bytes all; for (auto& c : cues_) {
        Bytes tp; cat(tp, eUint(0xF7, cfg_.hasAudio ? vTrackOrAudio() : vTrack())); cat(tp, eUint(0xF1, c.second));
        Bytes cp; cat(cp, eUint(0xB3, (uint64_t)c.first)); cat(cp, eElem(0xB7, tp)); cat(all, eElem(0xBB, cp)); }
      cues = eElem(0x1C53BB6B, all); }
    s = sink_->write(cues.data(), cues.size()); if (!s.ok()) return s;
    auto pos8 = [&](uint64_t at, uint64_t v) { Bytes b; for (int i = 7; i >= 0; --i) put8(b, (uint32_t)(v >> (8 * i))); return sink_->writeAt(at, b.data(), 8); };
    if (!(s = pos8(tagsSeekAt_, tagsPos - segData_)).ok()) return s;
    if (!(s = pos8(cuesSeekAt_, cuesPos - segData_)).ok()) return s;
    { Bytes d; double dur = (double)std::max<int64_t>(maxMs_, (int64_t)(seg.endNs - seg.startNs) / 1000000);
      beF64(d, dur); if (!(s = sink_->writeAt(durAt_, d.data(), 8)).ok()) return s; }
    { Bytes b; eSize8(b, sink_->size() - segData_); if (!(s = sink_->writeAt(segSizeAt_, b.data(), 8)).ok()) return s; }
    seg.videoFrames = videoCount_; return Status::Ok();
  }
 private:
  uint32_t vTrackOrAudio() const { return vTrack(); }
  uint64_t segSizeAt_ = 0, segData_ = 0, tagsSeekAt_ = 0, cuesSeekAt_ = 0, durAt_ = 0, durOffInInfo_ = 0, clSizeAt_ = 0, clPos_ = 0, clBytes_ = 0, pendStart_ = 0, nClusters_ = 0, videoCount_ = 0;
  int64_t clMs_ = 0, maxMs_ = 0; bool clOpen_ = false; Bytes pendA_; uint64_t pendFrames_ = 0;
  std::vector<std::pair<int64_t, uint64_t>> cues_;
};
} // namespace

std::unique_ptr<MediaWriter> MediaWriter::create(MediaContainer c, IFileSystem& fs, SegmentJournal* j) {
  if (c == MediaContainer::Avi) return std::unique_ptr<MediaWriter>(new AviWriter(fs, j));
  return std::unique_ptr<MediaWriter>(new MkvWriter(fs, j));
}

// =============================== AVI recovery ===============================
AviRecovery recoverAvi(IFileSystem& fs, const std::string& path) {
  AviRecovery r; Bytes b;
  if (!fs.readAll(path, b)) { r.detail = "cannot read file"; return r; }
  if (b.size() < 12 || !tagIs(b.data(), "RIFF") || !tagIs(b.data() + 8, "AVI ")) { r.detail = "not an AVI file"; return r; }
  size_t avihTotal = 0, vLen = 0, aLen = 0, moviSize = 0, moviTag = 0; uint32_t blockAlign = 0; int strlIndex = 0; bool vStream = false;
  // walk hdrl
  size_t pos = 12;
  while (pos + 12 <= b.size()) {
    uint32_t sz = get32(b.data() + pos + 4);
    if (tagIs(b.data() + pos, "LIST") && tagIs(b.data() + pos + 8, "movi")) { moviSize = pos + 4; moviTag = pos + 8; break; }
    if (tagIs(b.data() + pos, "LIST") && tagIs(b.data() + pos + 8, "hdrl")) {
      size_t p = pos + 12, end = std::min(b.size(), pos + 8 + (size_t)sz);
      while (p + 8 <= end) {
        uint32_t s2 = get32(b.data() + p + 4);
        if (tagIs(b.data() + p, "avih")) avihTotal = p + 8 + 16;
        else if (tagIs(b.data() + p, "LIST") && tagIs(b.data() + p + 8, "strl")) {
          size_t q = p + 12, qe = std::min(b.size(), p + 8 + (size_t)s2);
          while (q + 8 <= qe) {
            uint32_t s3 = get32(b.data() + q + 4);
            if (tagIs(b.data() + q, "strh") && q + 8 + 56 <= b.size()) {
              vStream = tagIs(b.data() + q + 8, "vids");
              if (vStream) vLen = q + 8 + 32; else aLen = q + 8 + 32;
            } else if (tagIs(b.data() + q, "strf") && !vStream && q + 8 + 16 <= b.size()) blockAlign = get16(b.data() + q + 8 + 12);
            q += 8 + (size_t)s3 + (s3 & 1);
          }
          ++strlIndex;
        }
        p += 8 + (size_t)s2 + (s2 & 1);
      }
    }
    pos += 8 + (size_t)sz + (sz & 1);
  }
  (void)strlIndex;
  if (!moviTag) { r.detail = "no movi list (truncated inside header)"; return r; }
  size_t p = moviTag + 4; size_t good = p; Bytes idx; uint64_t vc = 0, ac = 0; bool sawIdx = false; size_t idxAt = 0;
  while (p + 8 <= b.size()) {
    if (tagIs(b.data() + p, "idx1")) { sawIdx = true; idxAt = p; break; }
    const uint8_t* id = b.data() + p;
    bool v = id[0] == '0' && id[1] == '0' && id[2] == 'd' && id[3] == 'c';
    bool a = id[0] == '0' && id[1] == '1' && id[2] == 'w' && id[3] == 'b';
    if (!v && !a) break;
    uint32_t sz = get32(id + 4);
    size_t endc = p + 8 + (size_t)sz;
    if (endc > b.size()) break;
    if ((sz & 1)) { if (endc + 1 > b.size()) break; ++endc; }
    putBytes(idx, id, 4); put32(idx, (v ? (sz ? 0x10 : 0) : 0x10)); put32(idx, (uint32_t)(p - moviTag)); put32(idx, sz);
    if (v) ++vc; else if (blockAlign) ac += sz / blockAlign;
    p = endc; good = p;
  }
  r.videoSlots = vc; r.audioFrames = ac;
  uint32_t declaredRiff = get32(b.data() + 4);
  if (sawIdx && idxAt == good) {
    uint32_t isz = get32(b.data() + idxAt + 4);
    if (idxAt + 8 + isz == b.size() && declaredRiff == b.size() - 8) { r.ok = true; r.detail = "intact"; return r; }
  }
  size_t keep = sawIdx ? idxAt : good;   // an existing but inconsistent idx1 is rebuilt
  r.droppedTailBytes = b.size() - keep;
  Status st = fs.resize(path, keep);
  if (!st.ok()) { r.detail = "resize failed"; return r; }
  std::unique_ptr<IFileSink> sink; st = fs.open(path, IFileSystem::Mode::Update, sink);
  if (!st.ok()) { r.detail = "cannot open for repair"; return r; }
  Bytes ih; putTag(ih, "idx1"); put32(ih, (uint32_t)idx.size());
  Status w1 = sink->write(ih.data(), ih.size()); Status w2 = idx.empty() ? Status::Ok() : sink->write(idx.data(), idx.size());
  if (!w1.ok() || !w2.ok()) { r.detail = "cannot append idx1"; sink->close(); return r; }
  uint64_t total = sink->size();
  auto p32 = [&](size_t at, uint32_t v) { Bytes x; put32(x, v); return at ? sink->writeAt(at, x.data(), 4) : Status::Ok(); };
  p32(4, (uint32_t)(total - 8)); p32(moviSize, (uint32_t)(keep - moviTag)); p32(avihTotal, (uint32_t)vc); p32(vLen, (uint32_t)vc); p32(aLen, (uint32_t)ac);
  sink->close();
  r.ok = true; r.repaired = true; r.detail = "idx1 rebuilt by chunk scan";
  return r;
}

} // namespace ss
