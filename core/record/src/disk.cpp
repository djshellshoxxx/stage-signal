#include "ss/record/disk.hpp"
#include <algorithm>
#include <cmath>

namespace ss::record {

void RateEstimator::addSample(Ns now, std::uint64_t totalBytes) {
    if (!samples_.empty()) {
        if (now <= samples_.back().t || totalBytes < samples_.back().bytes) return;           // clock/ counter went backwards
        if (now - samples_.back().t < msToNs(500)) { samples_.back().t = now; samples_.back().bytes = totalBytes; return; }
    }
    samples_.push_back({now, totalBytes});
    if (samples_.size() > 120) samples_.pop_front();
}

RateEstimate RateEstimator::estimate() const {
    RateEstimate e;
    e.bytesPerSec = nominal_; e.relUncertainty = nominalUnc_; e.measured = false;
    if (samples_.size() < 2) return e;
    const Ns span = samples_.back().t - samples_.front().t;
    if (span < secToNs(5)) return e;
    // Mean over the window plus the scatter of ~1 s sub-interval rates -> uncertainty of the mean (about 95%).
    std::vector<double> rates;
    std::size_t anchor = 0;
    for (std::size_t i = 1; i < samples_.size(); ++i) {
        const Ns dt = samples_[i].t - samples_[anchor].t;
        if (dt >= secToNs(1)) {
            rates.push_back(static_cast<double>(samples_[i].bytes - samples_[anchor].bytes) / nsToSec(dt));
            anchor = i;
        }
    }
    const double mean = static_cast<double>(samples_.back().bytes - samples_.front().bytes) / nsToSec(span);
    double unc = 0.05;
    if (rates.size() >= 2 && mean > 0) {
        double m = 0; for (double r : rates) m += r; m /= static_cast<double>(rates.size());
        double var = 0; for (double r : rates) var += (r - m) * (r - m);
        var /= static_cast<double>(rates.size() - 1);
        unc = std::max(0.05, 2.0 * std::sqrt(var / static_cast<double>(rates.size())) / mean);
    }
    e.bytesPerSec = mean; e.relUncertainty = std::min(unc, 1.0); e.measured = true;
    return e;
}

std::uint64_t DiskMonitor::reserveFor(const RateEstimate& r) {
    const double thirtySec = r.upper() * nsToSec(Limits::kReserveNs);
    const double bytes = std::ceil(thirtySec);
    const std::uint64_t est = bytes >= 1.8e19 ? ~0ull : static_cast<std::uint64_t>(bytes);
    return std::max<std::uint64_t>(Limits::kReserveMinBytes, est);
}

DiskStatus DiskMonitor::update(std::uint64_t freeBytes) const {
    DiskStatus s;
    s.freeBytes = freeBytes;
    s.rate = rate_.estimate();
    s.reserveBytes = reserveFor(s.rate);
    const double rate = std::max(s.rate.bytesPerSec, 1.0);
    const double usable = freeBytes > s.reserveBytes ? static_cast<double>(freeBytes - s.reserveBytes) : 0.0;
    auto toNs = [](double sec) { return sec * 1e9 >= 9.0e18 ? static_cast<Ns>(9.0e18) : static_cast<Ns>(sec * 1e9); };
    s.remainingPointNs = toNs(usable / rate);
    s.remainingLowNs = toNs(usable / std::max(s.rate.upper(), 1.0));
    s.warnByFree = freeBytes <= Limits::kWarnFreeBytes;
    s.warnByTime = s.remainingLowNs <= Limits::kWarnRemainingNs;
    if (freeBytes <= s.reserveBytes) {
        s.level = DiskLevel::StopNow;
        s.message = "Disk space reached the stop reserve: stopping the recording now to close files cleanly.";
    } else if (s.warnByFree || s.warnByTime) {
        s.level = DiskLevel::Warning;
        const long mins = static_cast<long>(nsToSec(s.remainingPointNs) / 60.0);
        s.message = std::string("Disk space is low: about ") + std::to_string(mins) + " min of recording left (estimate " +
                    (s.rate.measured ? "from measured rate" : "from the profile, not yet measured") + ", +/-" +
                    std::to_string(static_cast<int>(s.rate.relUncertainty * 100)) + "%).";
    } else {
        s.level = DiskLevel::Ok;
    }
    return s;
}

namespace {
constexpr double kMjpegBytesPerPixel = 0.18;      // ~1.4 bit/pixel at -q:v 5: rough, replaced by the measured rate after 5 s
}

NominalRate nominalRateFor(const RecordProfile& p, VideoCodec codec) {
    const auto& s = p.spec();
    const double audio = static_cast<double>(s.audio.rateHz) * s.audio.channels * 3.0;      // 24-bit PCM
    NominalRate r;
    if (s.kind == ProfileKind::AudioOnlyWav) { r.bytesPerSec = audio; r.relUncertainty = 0.0; return r; }
    const double fps = static_cast<double>(s.video.fpsNum) / s.video.fpsDen;
    if (codec == VideoCodec::H264) { r.bytesPerSec = s.video.h264BitrateKbps * 125.0 + audio; r.relUncertainty = 0.25; }
    else { r.bytesPerSec = static_cast<double>(s.video.width) * s.video.height * fps * kMjpegBytesPerPixel + audio; r.relUncertainty = 0.6; }
    return r;
}

NominalRate nominalRate(const RecordProfile& p) {
    const auto& s = p.spec();
    if (s.kind == ProfileKind::AudioOnlyWav) return nominalRateFor(p, VideoCodec::H264);
    const NominalRate h = nominalRateFor(p, VideoCodec::H264), m = nominalRateFor(p, VideoCodec::Mjpeg);
    if (s.codecPolicy == VideoCodecPolicy::H264Only) return h;
    if (s.codecPolicy == VideoCodecPolicy::MjpegOnly) return m;
    return m.bytesPerSec >= h.bytesPerSec ? m : h;           // may fall back to MJPEG: plan for the larger
}

}  // namespace ss::record
