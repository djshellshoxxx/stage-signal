#include "ss/record/segment.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace ss::record {

AudioSegmenter::AudioSegmenter(std::uint32_t rateHz, Ns segmentNs, std::uint64_t sizeBudgetBytes,
                               std::uint32_t bytesPerFrame, std::uint64_t headerBytes)
    : rate_(rateHz) {
    const std::uint64_t byTime = static_cast<std::uint64_t>(segmentNs / kNsPerSec) * rateHz;       // whole seconds: exact
    const std::uint64_t budget = std::min<std::uint64_t>(sizeBudgetBytes, Limits::kWavSegmentMaxBytes);
    const std::uint64_t bySize = budget > headerBytes && bytesPerFrame ? (budget - headerBytes) / bytesPerFrame : 1;
    perSegment_ = std::max<std::uint64_t>(1, std::min(byTime, bySize));
    reason_ = byTime <= bySize ? CutReason::Time : CutReason::Size;
}

void AudioSegmenter::closeCurrent(CutReason r) {
    SegmentBound b;
    b.index = index_; b.startFrame = startFrame_; b.frames = inCurrent_; b.reason = r;
    b.startNs = samplesToNs(startFrame_, rate_);
    b.endNs = samplesToNs(startFrame_ + inCurrent_, rate_);
    closed_.push_back(b);
    startFrame_ += inCurrent_;
    inCurrent_ = 0;
    ++index_;
}

void AudioSegmenter::split(std::uint64_t frames, std::vector<Piece>& out) {
    while (frames > 0) {
        const std::uint64_t room = perSegment_ - inCurrent_;
        const std::uint64_t take = std::min(room, frames);
        Piece p;
        p.segmentIndex = index_; p.frames = take;
        inCurrent_ += take; frames -= take;
        if (inCurrent_ == perSegment_) { p.closesSegment = true; p.reason = reason_; closeCurrent(reason_); }
        out.push_back(p);
    }
}

SegmentBound AudioSegmenter::finish() {
    SegmentBound b;
    b.index = index_; b.startFrame = startFrame_; b.frames = inCurrent_; b.reason = CutReason::Stop;
    b.startNs = samplesToNs(startFrame_, rate_);
    b.endNs = samplesToNs(startFrame_ + inCurrent_, rate_);
    if (inCurrent_ > 0) closeCurrent(CutReason::Stop);
    return b;
}

Ns videoSegmentDuration(Ns segmentNs, std::uint64_t sizeBudgetBytes, double upperBytesPerSec) {
    Ns d = segmentNs;
    if (upperBytesPerSec > 0) {
        const double sec = static_cast<double>(sizeBudgetBytes) / upperBytesPerSec;
        const Ns bySize = sec * 1e9 >= 9.0e18 ? static_cast<Ns>(9.0e18) : static_cast<Ns>(std::floor(sec)) * kNsPerSec;
        d = std::min(d, bySize);
    }
    d = (d / kNsPerSec) * kNsPerSec;                       // whole seconds: ffmpeg -segment_time and keyframe expr stay exact
    return std::max<Ns>(d, Limits::kMinSegmentNs);
}

std::vector<SegmentListEntry> parseSegmentListCsv(const std::string& text, std::size_t* malformed) {
    std::vector<SegmentListEntry> out;
    std::size_t bad = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::size_t c2 = line.rfind(','), c1 = c2 == std::string::npos || c2 == 0 ? std::string::npos : line.rfind(',', c2 - 1);
        if (c1 == std::string::npos) { ++bad; continue; }
        char* e1 = nullptr; char* e2 = nullptr;
        const std::string a = line.substr(c1 + 1, c2 - c1 - 1), b = line.substr(c2 + 1);
        const double s = std::strtod(a.c_str(), &e1), e = std::strtod(b.c_str(), &e2);
        if (a.empty() || b.empty() || *e1 != '\0' || *e2 != '\0' || !(s >= 0) || !(e >= s)) { ++bad; continue; }
        SegmentListEntry en;
        en.file = line.substr(0, c1);
        if (en.file.size() >= 2 && en.file.front() == '"' && en.file.back() == '"') en.file = en.file.substr(1, en.file.size() - 2);
        en.startNs = static_cast<Ns>(std::llround(s * 1e9)); en.endNs = static_cast<Ns>(std::llround(e * 1e9));
        out.push_back(en);
    }
    if (malformed) *malformed = bad;
    return out;
}

std::vector<std::string> checkContinuity(const std::vector<SegmentListEntry>& v, Ns tol) {
    std::vector<std::string> problems;
    for (std::size_t i = 1; i < v.size(); ++i) {
        const Ns d = v[i].startNs - v[i - 1].endNs;
        if (d > tol) problems.push_back("gap of " + std::to_string(d / kNsPerMs) + " ms before segment " + std::to_string(i + 1));
        else if (d < -tol) problems.push_back("overlap of " + std::to_string(-d / kNsPerMs) + " ms before segment " + std::to_string(i + 1));
    }
    return problems;
}

}  // namespace ss::record
