#include "ss/dynamics/wav_writer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ss::dynamics {

namespace {
constexpr std::size_t kMaxGapEvents = 1024;

void put32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16); p[3] = static_cast<std::uint8_t>(v >> 24);
}
void put16(std::uint8_t* p, std::uint16_t v) { p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8); }

std::array<std::uint8_t, kWavHeaderBytes> makeHeader(std::uint32_t rate, std::uint16_t ch, std::uint32_t riffSize, std::uint32_t dataSize) {
    std::array<std::uint8_t, kWavHeaderBytes> h{};
    std::memcpy(&h[0], "RIFF", 4); put32(&h[4], riffSize); std::memcpy(&h[8], "WAVE", 4);
    std::memcpy(&h[12], "fmt ", 4); put32(&h[16], 16); put16(&h[20], 1); put16(&h[22], ch);
    put32(&h[24], rate); put32(&h[28], rate * ch * 3u); put16(&h[32], static_cast<std::uint16_t>(ch * 3u)); put16(&h[34], 24);
    std::memcpy(&h[36], "data", 4); put32(&h[40], dataSize);
    return h;
}

Error ioError(const char* what) { return makeError(ErrorCategory::DiskFull, what, Recoverability::OperatorAction); }
}  // namespace

// ---- sinks ---------------------------------------------------------------------------------------
Status MemorySink::write(const std::uint8_t* data, std::size_t n) {
    store_->insert(store_->end(), data, data + n);
    return okStatus();
}
Status MemorySink::patch(std::uint64_t offset, const std::uint8_t* data, std::size_t n) {
    if (offset + n > store_->size()) return makeError(ErrorCategory::InvalidArgument, "patch beyond written bytes");
    std::memcpy(store_->data() + offset, data, n);
    return okStatus();
}

struct FileSink::Impl { std::ofstream f; };
FileSink::FileSink() : impl_(new Impl) {}
FileSink::~FileSink() = default;

Status FileSink::open(const std::string& path) {
    impl_->f.open(std::filesystem::u8path(path), std::ios::binary | std::ios::out | std::ios::trunc);
    if (!impl_->f.good()) return ioError("cannot open recording file for writing");
    size_ = 0;
    return okStatus();
}
Status FileSink::write(const std::uint8_t* data, std::size_t n) {
    impl_->f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    if (!impl_->f.good()) return ioError("write failed (disk full or I/O error)");
    size_ += n;
    return okStatus();
}
Status FileSink::patch(std::uint64_t offset, const std::uint8_t* data, std::size_t n) {
    if (offset + n > size_) return makeError(ErrorCategory::InvalidArgument, "patch beyond written bytes");
    impl_->f.seekp(static_cast<std::streamoff>(offset));
    impl_->f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    impl_->f.seekp(0, std::ios::end);
    if (!impl_->f.good()) return ioError("header update failed (disk full or I/O error)");
    return okStatus();
}
Status FileSink::flush() {
    impl_->f.flush();
    return impl_->f.good() ? okStatus() : Status(ioError("flush failed (disk full or I/O error)"));
}
Status FileSink::close() {
    if (!impl_->f.is_open()) return okStatus();
    impl_->f.flush();
    const bool ok = impl_->f.good();
    impl_->f.close();
    return ok ? okStatus() : Status(ioError("close failed (disk full or I/O error)"));
}

Result<OpenedSink> MemorySinkFactory::open(const SegmentInfo& info) {
    if (failOpenAtIndex >= 0 && static_cast<std::uint32_t>(failOpenAtIndex) == info.index)
        return ioError("segment open failed (injected)");
    auto store = std::make_shared<std::vector<std::uint8_t>>();
    segments.push_back(store);
    char name[32];
    std::snprintf(name, sizeof name, "mem_%04u.wav", info.index);
    return OpenedSink{std::make_unique<MemorySink>(store), name};
}

Result<OpenedSink> FileSinkFactory::open(const SegmentInfo& info) {
    char suffix[24];
    std::snprintf(suffix, sizeof suffix, "_%04u.wav", info.index);
    const std::string name = base_ + suffix;
    auto sink = std::make_unique<FileSink>();
    Status st = sink->open((std::filesystem::u8path(dir_) / std::filesystem::u8path(name)).u8string());
    if (!st.ok()) return st.error();
    return OpenedSink{std::move(sink), name};
}

// ---- naming ----------------------------------------------------------------------------------------
std::string sanitizeShowName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (ok) out.push_back(c);
        else if ((c == ' ' || c == '.') && !out.empty() && out.back() != '-') out.push_back('-');
        if (out.size() >= 48) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "show" : out;
}

std::string formatUtcCompact(Ns wallUnixNs) {
    std::int64_t secs = wallUnixNs / kNsPerSec;
    if (wallUnixNs < 0 && wallUnixNs % kNsPerSec != 0) --secs;
    std::int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; --days; }
    // civil_from_days (Howard Hinnant)
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    char buf[128];
    std::snprintf(buf, sizeof buf, "%04lld%02lld%02lldT%02lld%02lld%02lldZ", static_cast<long long>(y), static_cast<long long>(m),
                  static_cast<long long>(d), static_cast<long long>(rem / 3600), static_cast<long long>(rem % 3600 / 60),
                  static_cast<long long>(rem % 60));
    return buf;
}

std::string makeRecordingBaseName(Ns wallUnixNs, const std::string& sessionId, const std::string& showName) {
    return formatUtcCompact(wallUnixNs) + "_" + sanitizeShowName(sessionId) + "_" + sanitizeShowName(showName);
}

// ---- writer ------------------------------------------------------------------------------------------
WavSegmentWriter::WavSegmentWriter(const WavWriterConfig& cfg, ISinkFactory& factory)
    : cfg_(cfg), factory_(factory), rng_(cfg.ditherSeed) {}

WavSegmentWriter::~WavSegmentWriter() { (void)stop(); }

Status WavSegmentWriter::fail(Error e) {
    if (!failed_) { failed_ = true; failure_ = e; }
    return e;
}

void WavSegmentWriter::notify() { if (onJournal && !journal_.empty()) onJournal(journal_.back()); }

Status WavSegmentWriter::start(std::uint64_t firstSampleIndex) {
    if (started_) return makeError(ErrorCategory::InvalidArgument, "writer already started");
    if (cfg_.channels < 1 || cfg_.channels > 2) return makeError(ErrorCategory::UnsupportedCapability, "WAV writer supports 1 or 2 channels");
    if (cfg_.sampleRateHz < 8000 || cfg_.sampleRateHz > 384000) return makeError(ErrorCategory::InvalidArgument, "sample rate out of range");
    if (!(cfg_.segmentSeconds > 0.0) || !std::isfinite(cfg_.segmentSeconds)) return makeError(ErrorCategory::InvalidArgument, "segment length must be positive");
    if (cfg_.bufferFrames < 64 || cfg_.bufferFrames > (1u << 20)) return makeError(ErrorCategory::InvalidArgument, "bufferFrames out of range 64..1048576");
    frameBytes_ = cfg_.channels * 3u;
    const std::uint64_t budget = std::min(cfg_.maxSegmentBytes, kRiffMaxFileBytes);
    if (budget <= kWavHeaderBytes + frameBytes_) return makeError(ErrorCategory::InvalidArgument, "segment byte budget too small");
    const std::uint64_t bySize = (budget - kWavHeaderBytes) / frameBytes_;
    const std::uint64_t byTime = static_cast<std::uint64_t>(std::llround(cfg_.segmentSeconds * cfg_.sampleRateHz));
    segLimit_ = std::max<std::uint64_t>(1, std::min(bySize, byTime));
    patchInterval_ = cfg_.headerPatchIntervalFrames ? cfg_.headerPatchIntervalFrames : 2ull * cfg_.sampleRateHz;
    buf_.assign(static_cast<std::size_t>(cfg_.bufferFrames) * frameBytes_, 0);
    recentGapStarts_.reserve(cfg_.maxGapsInWindow + 2u);
    streamStart_ = next_ = firstSampleIndex;
    started_ = true;
    return okStatus();
}

Status WavSegmentWriter::openSegment() {
    SegmentInfo info;
    info.index = segIndex_;
    info.firstSample = streamStart_ + static_cast<std::uint64_t>(segIndex_) * segLimit_;
    info.startNs = samplesToNs(info.firstSample - streamStart_, cfg_.sampleRateHz);
    auto r = factory_.open(info);
    if (!r.ok()) return fail(r.error());
    sink_ = std::move(r.value().sink);
    SegmentJournalEntry e;
    e.index = info.index; e.name = r.value().name; e.state = SegmentState::Active;
    e.firstSample = e.endSample = info.firstSample; e.startNs = e.endNs = info.startNs;
    journal_.push_back(e);
    const auto h = makeHeader(cfg_.sampleRateHz, cfg_.channels, 0xFFFFFFFFu, 0xFFFFFFFFu);   // sizes unknown until patched
    Status st = sink_->write(h.data(), h.size());
    if (!st.ok()) { journal_.back().state = SegmentState::Failed; sink_.reset(); return fail(st.error()); }
    segFrames_ = 0; patchedFrames_ = 0; bufLen_ = 0;
    notify();
    return okStatus();
}

Status WavSegmentWriter::patchHeader(std::uint64_t frames) {
    const std::uint64_t dataBytes = frames * frameBytes_;
    std::uint8_t b[4];
    put32(b, static_cast<std::uint32_t>(36 + dataBytes));
    Status st = sink_->patch(4, b, 4);
    if (!st.ok()) return st;
    put32(b, static_cast<std::uint32_t>(dataBytes));
    st = sink_->patch(40, b, 4);
    if (!st.ok()) return st;
    patchedFrames_ = frames;
    return okStatus();
}

Status WavSegmentWriter::drain() {
    if (bufLen_ > 0) {
        Status st = sink_->write(buf_.data(), bufLen_);
        bufLen_ = 0;
        if (!st.ok()) return st;
    }
    return okStatus();
}

Status WavSegmentWriter::closeSegment() {
    if (!sink_) return okStatus();
    SegmentJournalEntry& e = journal_.back();
    auto finish = [&]() -> Status {
        Status st = drain();
        if (st.ok()) st = patchHeader(segFrames_);
        if (st.ok()) st = sink_->flush();
        if (st.ok()) st = sink_->close();
        return st;
    };
    Status st = finish();
    e.frames = segFrames_;
    e.bytes = segFrames_ * frameBytes_;
    e.endSample = e.firstSample + segFrames_;
    e.endNs = samplesToNs(e.endSample - streamStart_, cfg_.sampleRateHz);
    e.flushedFrames = patchedFrames_;
    if (st.ok()) { e.state = SegmentState::Completed; e.headerFinalised = true; }
    else e.state = SegmentState::Failed;
    sink_.reset();
    ++segIndex_;
    segFrames_ = 0; patchedFrames_ = 0;
    notify();
    if (!st.ok()) return fail(st.error());
    return okStatus();
}

Status WavSegmentWriter::flush() {
    if (failed_) return failure_;
    if (!sink_) return okStatus();
    Status st = drain();
    if (st.ok()) st = patchHeader(segFrames_);
    if (st.ok()) st = sink_->flush();
    if (!st.ok()) { journal_.back().state = SegmentState::Failed; return fail(st.error()); }
    SegmentJournalEntry& e = journal_.back();
    e.frames = segFrames_; e.bytes = segFrames_ * frameBytes_; e.flushedFrames = patchedFrames_;
    e.endSample = e.firstSample + segFrames_;
    e.endNs = samplesToNs(e.endSample - streamStart_, cfg_.sampleRateHz);
    notify();
    return okStatus();
}

Status WavSegmentWriter::stop() {
    if (stopped_ || !started_) return okStatus();
    stopped_ = true;
    if (sink_) {
        const bool wasFailed = failed_;
        Status st = closeSegment();
        if (wasFailed) return failure_;
        return st;
    }
    return failed_ ? Status(failure_) : okStatus();
}

Status WavSegmentWriter::writeFrames(const float* x, std::uint64_t frames) {
    const std::uint16_t ch = cfg_.channels;
    while (frames > 0) {
        if (!sink_) { Status st = openSegment(); if (!st.ok()) return st; }
        std::uint64_t n = std::min(frames, segLimit_ - segFrames_);
        std::uint64_t done = 0;
        while (done < n) {
            std::size_t cap = (buf_.size() - bufLen_) / frameBytes_;
            if (cap == 0) {
                Status st = drain();
                if (st.ok() && segFrames_ - patchedFrames_ >= patchInterval_) st = patchHeader(segFrames_);
                if (!st.ok()) { journal_.back().state = SegmentState::Failed; return fail(st.error()); }
                continue;
            }
            const std::uint64_t c = std::min<std::uint64_t>(cap, n - done);
            std::uint8_t* out = buf_.data() + bufLen_;
            for (std::uint64_t f = 0; f < c; ++f) {
                for (std::uint16_t k = 0; k < ch; ++k) {
                    std::int32_t q = 0;
                    if (x) {
                        const float s = x[(done + f) * ch + k];
                        if (std::isfinite(s)) {
                            double v = static_cast<double>(s) * 8388608.0;
                            if (cfg_.dither) v += static_cast<double>(rng_.unit()) - static_cast<double>(rng_.unit());
                            v = std::floor(v + 0.5);
                            if (v > 8388607.0) { v = 8388607.0; ++clipped_; }
                            else if (v < -8388608.0) { v = -8388608.0; ++clipped_; }
                            q = static_cast<std::int32_t>(v);
                        }
                    }
                    out[0] = static_cast<std::uint8_t>(q & 0xFF);
                    out[1] = static_cast<std::uint8_t>((q >> 8) & 0xFF);
                    out[2] = static_cast<std::uint8_t>((q >> 16) & 0xFF);
                    out += 3;
                }
            }
            bufLen_ += static_cast<std::size_t>(c) * frameBytes_;
            segFrames_ += c;
            done += c;
        }
        totalFrames_ += n;
        frames -= n;
        if (x) x += n * ch;
        if (segFrames_ == segLimit_) { Status st = closeSegment(); if (!st.ok()) return st; }
    }
    return okStatus();
}

Status WavSegmentWriter::write(const float* x, std::uint32_t frames, std::uint64_t first, std::uint32_t audioFlags) {
    if (!started_) return makeError(ErrorCategory::InvalidArgument, "writer not started");
    if (failed_) return failure_;
    if (stopped_) return makeError(ErrorCategory::Cancelled, "writer stopped");
    if (audioFlags & 1u) ++discontinuityFlags_;   // kAudioDiscontinuity: the index jump (if any) is what we act on
    const std::uint32_t rate = cfg_.sampleRateHz;

    if (first < next_) {   // duplicate / overlapping delivery: keep the first copy, never write samples twice
        const std::uint64_t overlap = next_ - first;
        GapEvent g;
        g.kind = GapKind::OverlapTrimmed; g.startSample = first; g.frames = std::min<std::uint64_t>(overlap, frames);
        g.startNs = samplesToNs(first >= streamStart_ ? first - streamStart_ : 0, rate); g.durationNs = samplesToNs(g.frames, rate);
        g.segmentIndex = segIndex_;
        g.label = "overlap: " + std::to_string(g.frames) + " duplicate frames dropped";
        if (gaps_.size() < kMaxGapEvents) { gaps_.push_back(g); if (onGap) onGap(g); } else ++gapsDropped_;
        if (overlap >= frames) return okStatus();
        x += overlap * cfg_.channels; frames = static_cast<std::uint32_t>(frames - overlap); first = next_;
    } else if (first > next_) {
        const std::uint64_t gap = first - next_;
        const std::uint64_t maxGap = static_cast<std::uint64_t>(std::llround(cfg_.maxGapMs * 1e-3 * rate));
        GapEvent g;
        g.kind = GapKind::SilenceInserted; g.startSample = next_; g.frames = gap;
        g.startNs = samplesToNs(next_ - streamStart_, rate); g.durationNs = samplesToNs(gap, rate);
        g.segmentIndex = segIndex_;
        char lab[96];
        std::snprintf(lab, sizeof lab, "gap: %.1f ms of silence inserted", nsToMs(g.durationNs));
        g.label = lab;
        if (gap > maxGap) {
            g.label = "gap: " + std::to_string(static_cast<long long>(nsToMs(g.durationNs))) + " ms exceeds allowance, recording stopped";
            if (gaps_.size() < kMaxGapEvents) { gaps_.push_back(g); if (onGap) onGap(g); } else ++gapsDropped_;
            Status st = fail(makeError(ErrorCategory::TimestampInvalid, "audio gap longer than allowance; recording stopped", Recoverability::OperatorAction));
            (void)closeSegment();
            return st;
        }
        const std::uint64_t window = static_cast<std::uint64_t>(std::llround(cfg_.gapWindowSeconds * rate));
        recentGapStarts_.erase(std::remove_if(recentGapStarts_.begin(), recentGapStarts_.end(),
                                              [&](std::uint64_t s) { return s + window < next_; }), recentGapStarts_.end());
        recentGapStarts_.push_back(next_);
        if (gaps_.size() < kMaxGapEvents) { gaps_.push_back(g); if (onGap) onGap(g); } else ++gapsDropped_;
        if (recentGapStarts_.size() > cfg_.maxGapsInWindow) {
            Status st = fail(makeError(ErrorCategory::TimestampInvalid, "repeated audio gaps; recording stopped", Recoverability::OperatorAction));
            (void)closeSegment();
            return st;
        }
        Status st = writeFrames(nullptr, gap);
        if (!st.ok()) return st;
        if (journal_.size() > g.segmentIndex) ++journal_[g.segmentIndex].gapCount;
        next_ += gap;
    }
    if (frames == 0) return okStatus();
    Status st = writeFrames(x, frames);
    if (!st.ok()) return st;
    next_ += frames;
    return okStatus();
}

}  // namespace ss::dynamics
