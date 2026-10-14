#include "recording/wav_writer.h"
#include "recording/naming.h"
#include <algorithm>
namespace ss {
using namespace rec;

static Bytes makeHeader(uint32_t rate, uint32_t ch, uint32_t dataBytes) {
  Bytes h; h.reserve(44);
  putTag(h, "RIFF"); put32(h, 36 + dataBytes); putTag(h, "WAVE");
  putTag(h, "fmt "); put32(h, 16); put16(h, 1); put16(h, ch); put32(h, rate); put32(h, rate * ch * 3); put16(h, ch * 3); put16(h, 24);
  putTag(h, "data"); put32(h, dataBytes);
  return h;
}

WavWriter::~WavWriter() { if (opened_ && !closed_) close(); }

Status WavWriter::open(const WavConfig& c) {
  if (opened_) return Status::Fail(Err::InvalidArgument, "WavWriter already open");
  if (c.channels < 1 || c.channels > 2) return Status::Fail(Err::UnsupportedCapability, "WAV writer supports 1 or 2 channels");
  if (c.rate < 8000 || c.rate > 192000) return Status::Fail(Err::InvalidArgument, "unsupported sample rate");
  cfg_ = c;
  cfg_.maxSegmentBytes = std::min<uint64_t>(std::max<uint64_t>(cfg_.maxSegmentBytes, 4096), 3ull << 30);
  uint64_t bpf = cfg_.channels * 3;
  uint64_t byBytes = (cfg_.maxSegmentBytes - kHeaderBytes) / bpf;
  uint64_t byTime = nsToSamples(cfg_.maxSegmentNs, cfg_.rate);
  segCap_ = std::max<uint64_t>(1, std::min(byBytes, byTime ? byTime : byBytes));
  hdrEvery_ = cfg_.headerUpdateFrames ? cfg_.headerUpdateFrames : cfg_.rate;
  opened_ = true;
  Status s = openSegment();
  return s.ok() ? s : fail(s);
}

Status WavWriter::fail(const Status& s) {
  if (!failed_) { failed_ = true; err_ = s; }
  // best effort: make the active segment's header describe exactly what is on disk
  if (sink_) { patchHeader(); sink_->close(); sink_.reset(); if (!segs_.empty()) { segs_.back().complete = false; } }
  return err_;
}

Status WavWriter::openSegment() {
  SegmentInfo s;
  s.index = (int)segs_.size(); s.kind = "wav";
  s.path = joinPath(cfg_.dir, segmentName(cfg_.base, s.index, "wav"));
  s.firstSample = cfg_.firstSample + total_;
  s.startNs = cfg_.originNs + samplesToNs(s.firstSample - cfg_.firstSample, cfg_.rate);
  s.endNs = s.startNs; s.bytes = kHeaderBytes;
  Status st = fs_.open(s.path, IFileSystem::Mode::Truncate, sink_);
  if (!st.ok()) return st;
  Bytes h = makeHeader(cfg_.rate, cfg_.channels, 0);
  st = sink_->write(h.data(), h.size());
  if (!st.ok()) return st;
  sink_->flush();
  segs_.push_back(s); segFrames_ = 0; sinceHdr_ = 0;
  if (j_) { Status js = j_->segmentOpen(s); if (!js.ok()) return js; }
  return Status::Ok();
}

Status WavWriter::patchHeader() {
  if (!sink_) return Status::Ok();
  uint64_t bpf = cfg_.channels * 3;
  uint64_t bytes = sink_->size() > kHeaderBytes ? sink_->size() - kHeaderBytes : 0;
  uint64_t frames = bytes / bpf;
  uint32_t dataBytes = (uint32_t)(frames * bpf);
  Bytes sz; put32(sz, 36 + dataBytes);
  Bytes dz; put32(dz, dataBytes);
  Status a = sink_->writeAt(4, sz.data(), 4);
  Status b = sink_->writeAt(40, dz.data(), 4);
  Status f = sink_->flush();
  SegmentInfo& s = segs_.back();
  s.audioFrames = frames; s.bytes = sink_->size(); s.endNs = s.startNs + samplesToNs(frames, cfg_.rate);
  if (!a.ok()) return a;
  if (!b.ok()) return b;
  if (!f.ok()) return f;
  durable_ = s.firstSample - cfg_.firstSample + frames;
  return Status::Ok();
}

Status WavWriter::closeSegment(bool clean) {
  if (!sink_) return Status::Ok();
  Status p = patchHeader();
  Status c = sink_->close(); sink_.reset();
  SegmentInfo& s = segs_.back();
  s.complete = clean && p.ok() && c.ok();
  if (j_) j_->segmentClose(s);
  if (!p.ok()) return p;
  return c;
}

Status WavWriter::write(const float* in, size_t frames) {
  if (!opened_ || closed_) return Status::Fail(Err::InvalidArgument, "WavWriter not open");
  if (failed_) return err_;
  const size_t ch = cfg_.channels;
  while (frames) {
    if (segFrames_ >= segCap_) {
      Status c = closeSegment(true);
      if (!c.ok()) return fail(c);
      Status o = openSegment();
      if (!o.ok()) return fail(o);
    }
    size_t take = (size_t)std::min<uint64_t>(frames, segCap_ - segFrames_);
    scratch_.resize(take * ch * 3);
    uint8_t* o = scratch_.data();
    for (size_t i = 0; i < take * ch; ++i) if (putPcm24(o + i * 3, in[i])) ++clamped_;
    uint64_t before = sink_->size();
    Status st = sink_->write(scratch_.data(), scratch_.size());
    if (!st.ok()) {
      // partial write: count only whole frames that reached the file
      uint64_t got = sink_->size() - before; uint64_t whole = got / (ch * 3);
      segFrames_ += whole; total_ += whole;
      return fail(st);
    }
    in += take * ch; frames -= take; segFrames_ += take; total_ += take; sinceHdr_ += take;
    segs_.back().audioFrames = segFrames_;
    if (sinceHdr_ >= hdrEvery_) {
      sinceHdr_ = 0;
      Status p = patchHeader();
      if (!p.ok()) return fail(p);
    }
  }
  return Status::Ok();
}

Status WavWriter::close() {
  if (closed_) return failed_ ? err_ : Status::Ok();
  closed_ = true;
  if (!opened_) return Status::Ok();
  if (failed_) return err_;
  Status c = closeSegment(true);
  if (!c.ok()) { failed_ = true; err_ = c; }
  return c;
}

WavRecovery recoverWav(IFileSystem& fs, const std::string& path) {
  WavRecovery r;
  Bytes b;
  if (!fs.readAll(path, b)) { r.detail = "cannot read file"; return r; }
  if (b.size() < 12 || !tagIs(b.data(), "RIFF") || !tagIs(b.data() + 8, "WAVE")) { r.detail = "not a RIFF/WAVE file"; return r; }
  size_t pos = 12; uint32_t blockAlign = 0; size_t dataOff = 0; bool haveFmt = false;
  while (pos + 8 <= b.size()) {
    uint32_t sz = get32(b.data() + pos + 4);
    if (tagIs(b.data() + pos, "fmt ")) {
      if (pos + 8 + 16 > b.size()) break;
      blockAlign = get16(b.data() + pos + 8 + 12); haveFmt = true;
    } else if (tagIs(b.data() + pos, "data")) { dataOff = pos + 8; break; }
    pos += 8 + (size_t)sz + (sz & 1);
  }
  if (!haveFmt || !blockAlign) { r.detail = "missing/invalid fmt chunk"; return r; }
  if (!dataOff) { r.detail = "no data chunk (header-only/truncated before data)"; return r; }
  uint64_t actual = b.size() - dataOff;
  uint64_t keep = actual / blockAlign * blockAlign;
  r.droppedTailBytes = actual - keep; r.dataBytes = keep; r.frames = keep / blockAlign;
  uint32_t declaredData = get32(b.data() + dataOff - 4), declaredRiff = get32(b.data() + 4);
  bool sizesOk = declaredData == keep && declaredRiff == dataOff - 8 + keep && r.droppedTailBytes == 0;
  r.ok = true;
  if (sizesOk) { r.detail = "intact"; return r; }
  if (keep > 0xFFFFFFF0ull) { r.ok = false; r.detail = "data exceeds classic RIFF limit"; return r; }
  Bytes d, rz; put32(d, (uint32_t)keep); put32(rz, (uint32_t)(dataOff - 8 + keep));
  std::unique_ptr<IFileSink> sink;
  Status st = fs.open(path, IFileSystem::Mode::Update, sink);
  if (!st.ok()) { r.ok = false; r.detail = "cannot open for repair: " + st.detail; return r; }
  Status a = sink->writeAt(4, rz.data(), 4), c = sink->writeAt(dataOff - 4, d.data(), 4);
  sink->close();
  if (!a.ok() || !c.ok()) { r.ok = false; r.detail = "header patch failed"; return r; }
  if (r.droppedTailBytes) fs.resize(path, dataOff + keep);
  r.repaired = true; r.detail = "sizes repaired from file length";
  return r;
}

} // namespace ss
