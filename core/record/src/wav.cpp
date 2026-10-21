#include "ss/record/wav.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ss::record {
namespace {
void put16(std::string& s, std::uint32_t v) { s.push_back(char(v & 0xFF)); s.push_back(char((v >> 8) & 0xFF)); }
void put32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xFF)); }
std::uint32_t get16(const std::string& s, std::size_t at) { return std::uint8_t(s[at]) | (std::uint32_t(std::uint8_t(s[at + 1])) << 8); }
std::uint32_t get32(const std::string& s, std::size_t at) { return get16(s, at) | (get16(s, at + 2) << 16); }
Error diskErr(const char* what) { return makeError(ErrorCategory::DiskFull, what, Recoverability::OperatorAction); }

int seek64(FILE* f, std::uint64_t off, int whence) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(off), whence);
#else
    return fseeko(f, static_cast<off_t>(off), whence);
#endif
}
Status syncFile(FILE* f) {
    if (std::fflush(f) != 0) return diskErr("write failed (disk full or device error)");
#ifdef _WIN32
    if (_commit(_fileno(f)) != 0) return diskErr("could not flush to stable storage");
#else
    if (fsync(fileno(f)) != 0) return diskErr("could not flush to stable storage");
#endif
    return okStatus();
}

class StdioFile final : public ISegmentFile {
public:
    explicit StdioFile(FILE* f) : f_(f) {}
    ~StdioFile() override { if (f_) std::fclose(f_); }
    Status write(const void* d, std::size_t n) override {
        if (!f_) return makeError(ErrorCategory::InternalFault, "file is closed");
        if (n && std::fwrite(d, 1, n, f_) != n) return diskErr("write failed (disk full or device error)");
        pos_ += n;
        return okStatus();
    }
    Status patch(std::uint64_t off, const void* d, std::size_t n) override {
        if (!f_) return makeError(ErrorCategory::InternalFault, "file is closed");
        if (std::fflush(f_) != 0 || seek64(f_, off, SEEK_SET) != 0 || std::fwrite(d, 1, n, f_) != n || std::fflush(f_) != 0 ||
            seek64(f_, pos_, SEEK_SET) != 0)
            return diskErr("could not update the file header");
        return okStatus();
    }
    Status flushDurable() override { return f_ ? syncFile(f_) : makeError(ErrorCategory::InternalFault, "file is closed"); }
    Status close() override {
        if (!f_) return okStatus();
        Status s = syncFile(f_);
        const int rc = std::fclose(f_);
        f_ = nullptr;
        if (!s.ok()) return s;
        return rc == 0 ? okStatus() : diskErr("close failed");
    }
private:
    FILE* f_;
    std::uint64_t pos_ = 0;
};
}  // namespace

std::string makeWavHeader(std::uint32_t rate, std::uint32_t ch, std::uint64_t dataBytes) {
    const std::uint64_t data = std::min<std::uint64_t>(dataBytes, 0xFFFFFFFFull - 60);
    std::string h = "RIFF";
    put32(h, static_cast<std::uint32_t>(60 + data));
    h += "WAVE";
    h += "fmt "; put32(h, 40);
    put16(h, 0xFFFE); put16(h, ch); put32(h, rate); put32(h, rate * ch * 3); put16(h, ch * 3); put16(h, 24);
    put16(h, 22); put16(h, 24); put32(h, ch == 1 ? 0x4 : (ch == 2 ? 0x3 : 0));
    static const unsigned char pcmGuid[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    h.append(reinterpret_cast<const char*>(pcmGuid), 16);
    h += "data"; put32(h, static_cast<std::uint32_t>(data));
    return h;
}

Result<WavHeaderInfo> parseWavHeader(const std::string& p) {
    auto bad = [](const char* w) { return makeError(ErrorCategory::DecodeFailure, w, Recoverability::OperatorAction); };
    if (p.size() < 44 || p.compare(0, 4, "RIFF") != 0 || p.compare(8, 4, "WAVE") != 0) return bad("not a WAV file");
    WavHeaderInfo info;
    bool haveFmt = false;
    std::size_t pos = 12;
    while (pos + 8 <= p.size()) {
        const std::string id = p.substr(pos, 4);
        const std::uint32_t sz = get32(p, pos + 4);
        if (id == "fmt ") {
            if (sz < 16 || pos + 8 + 16 > p.size()) return bad("fmt chunk truncated");
            const std::uint32_t tag = get16(p, pos + 8);
            if (tag != 1 && tag != 0xFFFE) return bad("not PCM");
            info.channels = get16(p, pos + 10); info.rateHz = get32(p, pos + 12);
            info.blockAlign = get16(p, pos + 20); info.bitsPerSample = get16(p, pos + 22);
            haveFmt = true;
        } else if (id == "data") {
            if (!haveFmt) return bad("data before fmt");
            info.dataOffset = pos + 8; info.declaredDataBytes = sz;
            if (info.channels == 0 || info.rateHz == 0 || info.blockAlign == 0 || info.bitsPerSample != 24 ||
                info.blockAlign != info.channels * 3)
                return bad("unsupported or inconsistent PCM format");
            return info;
        }
        const std::uint64_t next = pos + 8 + static_cast<std::uint64_t>(sz) + (sz & 1);
        if (next > p.size()) break;
        pos = static_cast<std::size_t>(next);
    }
    return bad("data chunk not found");
}

Result<WavRepair> planWavRepair(const WavHeaderInfo& info, std::uint64_t fileSize) {
    WavRepair r;
    const std::uint64_t avail = fileSize > info.dataOffset ? fileSize - info.dataOffset : 0;
    r.dataBytes = (avail / info.blockAlign) * info.blockAlign;
    if (r.dataBytes > 0xFFFFFFFFull - 60) return makeError(ErrorCategory::ResourceExhausted, "WAV data exceeds the classic RIFF limit");
    r.droppedPartialFrame = r.dataBytes != avail;
    r.truncateTo = info.dataOffset + r.dataBytes;
    r.headerWasWrong = info.declaredDataBytes != r.dataBytes;
    return r;
}

void pcm24FromFloat(const float* in, std::size_t n, std::uint8_t* out) {
    for (std::size_t i = 0; i < n; ++i) {
        float f = in[i];
        std::int32_t v = 0;
        if (f == f) {                                         // NaN -> 0
            if (f > 1.0f) f = 1.0f; else if (f < -1.0f) f = -1.0f;
            v = static_cast<std::int32_t>(std::lrintf(f * 8388607.0f));
        }
        out[i * 3 + 0] = static_cast<std::uint8_t>(v & 0xFF);
        out[i * 3 + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        out[i * 3 + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    }
}

// ---- StdioRecorderFs ---------------------------------------------------------------------------------------------
std::string StdioRecorderFs::path(const std::string& name) const {
    if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name.find("..") != std::string::npos)
        return std::string();
    return (std::filesystem::path(dir_) / name).string();
}
Result<std::unique_ptr<ISegmentFile>> StdioRecorderFs::createFile(const std::string& name) {
    const std::string p = path(name);
    if (p.empty()) return makeError(ErrorCategory::InvalidArgument, "invalid file name");
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return makeError(ErrorCategory::PermissionDenied, "cannot create the recording file", Recoverability::OperatorAction);
    return std::unique_ptr<ISegmentFile>(new StdioFile(f));
}
bool StdioRecorderFs::fileSize(const std::string& name, std::uint64_t& size) {
    const std::string p = path(name);
    std::error_code ec;
    if (p.empty()) return false;
    const auto s = std::filesystem::file_size(p, ec);
    if (ec) return false;
    size = s;
    return true;
}
Result<std::string> StdioRecorderFs::readPrefix(const std::string& name, std::size_t n) {
    const std::string p = path(name);
    FILE* f = p.empty() ? nullptr : std::fopen(p.c_str(), "rb");
    if (!f) return makeError(ErrorCategory::PermissionDenied, "cannot read the file");
    std::string buf(n, '\0');
    buf.resize(std::fread(&buf[0], 1, n, f));
    std::fclose(f);
    return buf;
}
Status StdioRecorderFs::patch(const std::string& name, std::uint64_t offset, const std::string& bytes) {
    const std::string p = path(name);
    FILE* f = p.empty() ? nullptr : std::fopen(p.c_str(), "r+b");
    if (!f) return makeError(ErrorCategory::PermissionDenied, "cannot open the file for repair");
    const bool ok = seek64(f, offset, SEEK_SET) == 0 && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    return ok && closed ? okStatus() : Status(diskErr("could not repair the file"));
}
Status StdioRecorderFs::truncate(const std::string& name, std::uint64_t newSize) {
    const std::string p = path(name);
    std::error_code ec;
    if (p.empty()) return makeError(ErrorCategory::InvalidArgument, "invalid file name");
    std::filesystem::resize_file(p, newSize, ec);
    return ec ? Status(diskErr("could not truncate the file")) : okStatus();
}

// ---- AudioOnlyRecorder -------------------------------------------------------------------------------------------
AudioOnlyRecorder::AudioOnlyRecorder(const RecordProfile& profile, std::string sessionId, Ns wallStart, IRecorderFs& fs,
                                     IJournalSink& journal)
    : profile_(profile), sessionId_(std::move(sessionId)), wallStart_(wallStart), fs_(fs), journal_(journal),
      seg_(profile.spec().audio.rateHz, profile.spec().segmentNs, profile.effectiveSizeBudget(), profile.spec().audio.channels * 3,
           kWavHeaderBytes),
      rate_(profile.spec().audio.rateHz), channels_(profile.spec().audio.channels), bytesPerFrame_(profile.spec().audio.channels * 3) {}

Status AudioOnlyRecorder::start() {
    if (running_) return makeError(ErrorCategory::InvalidArgument, "recorder already started");
    if (profile_.spec().kind != ProfileKind::AudioOnlyWav)
        return makeError(ErrorCategory::InvalidArgument, "profile is not an audio-only profile");
    Status s = journal_.begin(sessionId_, profile_.hash(), ProfileKind::AudioOnlyWav, wallStart_ / kNsPerMs);
    if (!s.ok()) return s;
    running_ = true;
    return openSegment(seg_.currentIndex());
}

Status AudioOnlyRecorder::openSegment(std::uint32_t idx) {
    const std::string name = segmentFileName(wallStart_, sessionId_, profile_.spec().showName, idx, "wav");
    auto f = fs_.createFile(name);
    if (!f.ok()) return f.error();
    file_ = std::move(f.value());
    const std::string h = makeWavHeader(rate_, channels_, 0);
    Status s = file_->write(h.data(), h.size());
    if (!s.ok()) return s;
    segBytes_ = h.size();
    totalBytes_ += h.size();
    files_.push_back(name);
    openIndex_ = idx;
    return journal_.segmentOpened(idx, name, samplesToNs(framesWritten_, rate_));
}

Status AudioOnlyRecorder::flushNow() {
    if (!file_) return okStatus();
    const std::string h = makeWavHeader(rate_, channels_, segBytes_ - kWavHeaderBytes);
    Status s = file_->patch(0, h.data(), h.size());
    if (!s.ok()) return s;
    s = file_->flushDurable();
    if (!s.ok()) return s;
    sinceFlushFrames_ = 0;
    return journal_.progress(openIndex_, segBytes_, mediaNs());
}

Status AudioOnlyRecorder::closeSegment(Ns endNs) {
    if (!file_) return okStatus();
    const std::string h = makeWavHeader(rate_, channels_, segBytes_ - kWavHeaderBytes);
    Status s = file_->patch(0, h.data(), h.size());
    if (s.ok()) s = file_->close();
    const std::uint32_t idx = openIndex_;
    file_.reset();
    if (!s.ok()) return s;
    sinceFlushFrames_ = 0;
    const std::uint64_t bytes = segBytes_;
    segBytes_ = 0;
    return journal_.segmentClosed(idx, bytes, endNs);
}

Status AudioOnlyRecorder::writeFrames(const std::uint8_t* pcm, std::uint64_t frames) {
    pieces_.clear();
    seg_.split(frames, pieces_);
    std::uint64_t offset = 0;
    for (const auto& p : pieces_) {
        if (!file_) {
            // The segmenter already advanced its index when it closed the previous piece; the new file takes this piece's index.
            Status s = openSegment(p.segmentIndex);
            if (!s.ok()) return s;
        }
        const std::uint64_t n = p.frames * bytesPerFrame_;
        Status s = file_->write(pcm + offset, static_cast<std::size_t>(n));
        if (!s.ok()) return s;
        offset += n; segBytes_ += n; totalBytes_ += n;
        framesWritten_ += p.frames; sinceFlushFrames_ += p.frames;
        if (p.closesSegment) {
            s = closeSegment(samplesToNs(framesWritten_, rate_));
            if (!s.ok()) return s;
        }
    }
    if (file_ && sinceFlushFrames_ >= rate_) return flushNow();
    return okStatus();
}

Status AudioOnlyRecorder::write(const AudioBlockView& b) {
    if (!running_) return makeError(ErrorCategory::Cancelled, "recorder is not running");
    if (b.frames == 0) return okStatus();
    if (!b.interleaved || b.channels != channels_ || b.rateHz != rate_ || b.frames > (1u << 20))
        return makeError(ErrorCategory::InvalidArgument, "audio block does not match the recording profile");
    const float* p = b.interleaved;
    std::uint64_t frames = b.frames;
    if (!haveExpected_) { haveExpected_ = true; generation_ = b.generation; expectedIndex_ = b.firstSampleIndex; }
    else if (b.generation != generation_) {
        generation_ = b.generation; expectedIndex_ = b.firstSampleIndex; ++gaps_;
        (void)journal_.gap(mediaNs(), 0, "capture restarted; length unknown");
    } else if (b.firstSampleIndex > expectedIndex_) {
        const std::uint64_t gap = b.firstSampleIndex - expectedIndex_;
        const std::uint64_t fill = std::min<std::uint64_t>(gap, static_cast<std::uint64_t>(rate_) * 10);
        ++gaps_;
        (void)journal_.gap(mediaNs(), samplesToNs(gap, rate_), "capture discontinuity");
        // Keep the timeline: write silence for the missing frames (bounded to 10 s per gap).
        std::uint64_t left = fill;
        while (left > 0) {
            const std::uint64_t n = std::min<std::uint64_t>(left, 16384);
            scratch_.assign(static_cast<std::size_t>(n * bytesPerFrame_), 0);
            Status s = writeFrames(scratch_.data(), n);
            if (!s.ok()) return s;
            left -= n; silenceFrames_ += n;
        }
    } else if (b.firstSampleIndex < expectedIndex_) {
        const std::uint64_t overlap = expectedIndex_ - b.firstSampleIndex;
        if (overlap >= frames) return okStatus();                       // entirely repeated data
        p += overlap * channels_; frames -= overlap;
    }
    expectedIndex_ = b.firstSampleIndex + b.frames;
    scratch_.resize(static_cast<std::size_t>(frames * bytesPerFrame_));
    pcm24FromFloat(p, static_cast<std::size_t>(frames * channels_), scratch_.data());
    return writeFrames(scratch_.data(), frames);
}

Status AudioOnlyRecorder::stop(bool clean) {
    if (!running_) return okStatus();
    running_ = false;
    Status first = okStatus();
    const SegmentBound last = seg_.finish();
    if (file_) {
        Status s = closeSegment(last.endNs);
        if (!s.ok()) first = s;
    }
    Status e = journal_.end(clean && first.ok(), first.ok() ? (clean ? "stopped" : "stopped after fault") : "close failed", mediaNs());
    return first.ok() ? e : first;
}

}  // namespace ss::record
