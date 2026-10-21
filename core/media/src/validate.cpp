#include "ss/media/validate.hpp"
#include <algorithm>
#include <cmath>
#include "ss/base/utf8.hpp"

namespace ss::media {
namespace {
Error reject(ErrorCategory c, std::string why) { return makeError(c, std::move(why), Recoverability::OperatorAction); }
std::uint64_t rgbaBytes(std::uint32_t w, std::uint32_t h) {
    return static_cast<std::uint64_t>(w) * h * Limits::kBytesPerPixel;       // w,h <= 4096 when called: no overflow
}
}  // namespace

void fitWithin(std::uint32_t w, std::uint32_t h, std::uint32_t maxW, std::uint32_t maxH,
               std::uint32_t& outW, std::uint32_t& outH) {
    if (w <= maxW && h <= maxH) { outW = w; outH = h; return; }
    // scale = min(maxW/w, maxH/h) in integer math: compare maxW*h with maxH*w
    if (static_cast<std::uint64_t>(maxW) * h <= static_cast<std::uint64_t>(maxH) * w) {
        outW = maxW;
        outH = std::max<std::uint32_t>(1, static_cast<std::uint32_t>((static_cast<std::uint64_t>(h) * maxW) / w));
    } else {
        outH = maxH;
        outW = std::max<std::uint32_t>(1, static_cast<std::uint32_t>((static_cast<std::uint64_t>(w) * maxH) / h));
    }
}

Result<StillPlan> validateStill(std::uint32_t w, std::uint32_t h, Tier tier, const DecodedCacheBudget* cache) {
    if (w == 0 || h == 0) return reject(ErrorCategory::InvalidArgument, "still has zero dimensions");
    if (w > Limits::kMaxStillDim || h > Limits::kMaxStillDim)
        return reject(ErrorCategory::ResourceExhausted, "still exceeds 4096 pixels on a side; resize it and import again");
    if (rgbaBytes(w, h) > Limits::kMaxStillDecodedBytes)
        return reject(ErrorCategory::ResourceExhausted, "still would decode to more than 64 MiB");
    StillPlan p;
    p.srcWidth = w; p.srcHeight = h; p.targetWidth = w; p.targetHeight = h;
    if (tier == Tier::Base && (w > Limits::kBaseStillW || h > Limits::kBaseStillH)) {
        fitWithin(w, h, Limits::kBaseStillW, Limits::kBaseStillH, p.targetWidth, p.targetHeight);
        p.needsDownscale = true;
        p.warnings.push_back("BASE tier: still is preprocessed off the live path to fit 800x600");
    }
    p.decodedBytes = rgbaBytes(p.targetWidth, p.targetHeight);
    if (tier == Tier::Base) {
        const std::uint64_t room = cache ? cache->remaining() : Limits::kBaseDecodedCacheBytes;
        if (p.decodedBytes > room)
            return reject(ErrorCategory::ResourceExhausted, "BASE decoded media cache (8 MiB) would be exceeded; remove other media first");
    }
    return p;
}

GifTiming normalizeGifDelays(const std::vector<std::uint32_t>& raw) {
    GifTiming t;
    t.delaysMs.reserve(raw.size());
    for (std::uint32_t d : raw) {
        if (d < Limits::kMinGifDelayMs) { t.delaysMs.push_back(Limits::kMinGifDelayMs); ++t.clampedCount; }
        else t.delaysMs.push_back(d);
    }
    if (t.clampedCount)
        t.warnings.push_back(std::to_string(t.clampedCount) + " GIF frame delay(s) below 20 ms were clamped to 20 ms");
    return t;
}

Result<GifPlan> validateGif(const GifInfo& info, Tier tier, const DecodedCacheBudget* cache) {
    if (info.frameCapExceeded || info.frames.size() > Limits::kMaxGifFrames)
        return reject(ErrorCategory::ResourceExhausted, "GIF has more than 1000 frames");
    if (info.frames.empty()) return reject(ErrorCategory::DecodeFailure, "GIF has no frames");
    if (info.width == 0 || info.height == 0) return reject(ErrorCategory::InvalidArgument, "GIF has zero dimensions");
    if (info.width > Limits::kMaxStillDim || info.height > Limits::kMaxStillDim)
        return reject(ErrorCategory::ResourceExhausted, "GIF canvas exceeds 4096 pixels on a side");
    if (tier == Tier::Base && (info.width > Limits::kBaseStillW || info.height > Limits::kBaseStillH))
        return reject(ErrorCategory::ResourceExhausted, "BASE tier: GIF canvas must be 800x600 or smaller (re-export it smaller)");
    GifPlan p;
    p.width = info.width; p.height = info.height;
    p.frameCount = static_cast<std::uint32_t>(info.frames.size());
    p.frameBytes = rgbaBytes(info.width, info.height);
    if (p.frameBytes > Limits::kMaxStillDecodedBytes)
        return reject(ErrorCategory::ResourceExhausted, "GIF canvas would decode to more than 64 MiB per frame");
    const std::uint64_t cap = tier == Tier::Base ? (cache ? cache->remaining() : Limits::kBaseDecodedCacheBytes)
                                                 : Limits::kGifStreamCacheBytes;
    const std::uint64_t fit = cap / p.frameBytes;
    const std::uint64_t need = std::min<std::uint64_t>(p.frameCount, Limits::kGifMinFramesInCache);
    if (fit < need) return reject(ErrorCategory::ResourceExhausted, "GIF streamed cache budget cannot hold the minimum working frames");
    p.cacheFrames = static_cast<std::uint32_t>(std::min<std::uint64_t>(p.frameCount, fit));
    p.cacheBytes = p.frameBytes * p.cacheFrames;
    std::vector<std::uint32_t> raw;
    raw.reserve(info.frames.size());
    for (const auto& f : info.frames) raw.push_back(f.rawDelayMs);
    p.timing = normalizeGifDelays(raw);
    p.loopCount = info.loopCount;
    return p;
}

GifTimeline::GifTimeline(const std::vector<std::uint32_t>& delaysMs, int loopCount) : loops_(loopCount) {
    std::uint64_t acc = 0;
    cumMs_.reserve(delaysMs.size());
    for (std::uint32_t d : delaysMs) { acc += std::max<std::uint32_t>(d, Limits::kMinGifDelayMs); cumMs_.push_back(acc); }
    cycleNs_ = static_cast<Ns>(acc) * kNsPerMs;
}

std::uint32_t GifTimeline::frameAt(Ns elapsedWallNs, double speed, bool* finished) const {
    if (finished) *finished = false;
    if (cumMs_.empty() || cycleNs_ <= 0) return 0;
    if (elapsedWallNs < 0 || !(speed > 0)) elapsedWallNs = 0;
    const double mediaD = static_cast<double>(elapsedWallNs) * speed;
    const Ns media = mediaD >= 9.0e18 ? static_cast<Ns>(9.0e18) : static_cast<Ns>(mediaD);
    // No NETSCAPE block (-1): play once. 0: forever. n: n extra plays after the first.
    const std::int64_t plays = loops_ == 0 ? 0 : (loops_ < 0 ? 1 : static_cast<std::int64_t>(loops_) + 1);
    if (plays > 0 && media / cycleNs_ >= plays) {
        if (finished) *finished = true;
        return frameCount() - 1;
    }
    const Ns inCycle = media % cycleNs_;
    const std::uint64_t ms = static_cast<std::uint64_t>(inCycle / kNsPerMs);
    auto it = std::upper_bound(cumMs_.begin(), cumMs_.end(), ms);
    return it == cumMs_.end() ? frameCount() - 1 : static_cast<std::uint32_t>(it - cumMs_.begin());
}

Status validateSpeed(SpeedKind kind, double s) {
    const double lo = kind == SpeedKind::Video ? Limits::kVideoSpeedMin : Limits::kGifSpeedMin;
    const double hi = kind == SpeedKind::Video ? Limits::kVideoSpeedMax : Limits::kGifSpeedMax;
    if (!std::isfinite(s) || s < lo || s > hi)
        return reject(ErrorCategory::InvalidArgument, kind == SpeedKind::Video ? "video speed must be 0.25x to 2x"
                                                                                : "GIF speed must be 0.25x to 4x");
    return okStatus();
}
double clampSpeed(SpeedKind kind, double s) {
    if (!std::isfinite(s)) return 1.0;
    const double lo = kind == SpeedKind::Video ? Limits::kVideoSpeedMin : Limits::kGifSpeedMin;
    const double hi = kind == SpeedKind::Video ? Limits::kVideoSpeedMax : Limits::kGifSpeedMax;
    return std::min(hi, std::max(lo, s));
}

Result<VideoPlan> validateVideo(const VideoInfo& v, Tier tier) {
    if (!isVideoContainer(v.container)) return reject(ErrorCategory::InvalidArgument, "not an allowed video container");
    if (tier == Tier::Base)
        return reject(ErrorCategory::UnsupportedCapability, "video playback needs STANDARD tier or higher; BASE shows stills/GIF/procedural scenes");
    if (v.videoCodec != "h264" && v.videoCodec != "mjpeg")
        return reject(ErrorCategory::UnsupportedCapability, "video codec is not on the declared allowlist (H.264, MJPEG)");
    if (v.width == 0 || v.height == 0 || v.width > Limits::kMaxVideoDim || v.height > Limits::kMaxVideoDim)
        return reject(ErrorCategory::ResourceExhausted, "video dimensions are outside 1..4096");
    if (rgbaBytes(v.width, v.height) > Limits::kMaxStillDecodedBytes)
        return reject(ErrorCategory::ResourceExhausted, "one decoded video frame would exceed 64 MiB");
    if (!(v.durationSec > 0) || !std::isfinite(v.durationSec) || !(v.fps > 0) || v.fps > 240)
        return reject(ErrorCategory::InvalidArgument, "video duration or frame rate is invalid");
    VideoPlan p; p.width = v.width; p.height = v.height; p.frameBytes = rgbaBytes(v.width, v.height);
    return p;
}

Status validateText(const std::string& s) {
    const long n = utf8::countCodePoints(s);
    if (n < 0) return reject(ErrorCategory::InvalidArgument, "text is not valid UTF-8");
    if (static_cast<std::size_t>(n) > Limits::kMaxTextCodePoints)
        return reject(ErrorCategory::ResourceExhausted, "text exceeds 4096 code points");
    if (s.find('\0') != std::string::npos) return reject(ErrorCategory::InvalidArgument, "text contains a NUL character");
    return okStatus();
}
Status validateMessageCount(std::size_t count) {
    if (count > Limits::kMaxMessages) return reject(ErrorCategory::ResourceExhausted, "more than 256 messages in a show");
    return okStatus();
}

Ns loopPosition(Ns elapsedWallNs, double speed, Ns durationNs, bool loop, bool* ended) {
    if (ended) *ended = false;
    if (durationNs <= 0) return 0;
    if (elapsedWallNs < 0 || !(speed > 0)) elapsedWallNs = 0;
    const double mediaD = static_cast<double>(elapsedWallNs) * speed;
    const Ns media = mediaD >= 9.0e18 ? static_cast<Ns>(9.0e18) : static_cast<Ns>(mediaD);
    if (loop) return media % durationNs;
    if (media >= durationNs) { if (ended) *ended = true; return durationNs; }
    return media;
}

Status validateCrop(const CropRect& c) {
    const float eps = 1e-4f;
    if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.w) || !std::isfinite(c.h))
        return reject(ErrorCategory::InvalidArgument, "crop contains a non-finite value");
    if (c.x < 0 || c.y < 0 || c.w <= 0 || c.h <= 0 || c.x + c.w > 1 + eps || c.y + c.h > 1 + eps)
        return reject(ErrorCategory::InvalidArgument, "crop rectangle must lie inside the source (normalised 0..1, non-empty)");
    return okStatus();
}

FitResult computeFit(std::uint32_t sw, std::uint32_t sh, std::uint32_t dw, std::uint32_t dh, FitMode mode) {
    FitResult r;
    r.src = {0, 0, static_cast<std::int32_t>(sw), static_cast<std::int32_t>(sh)};
    r.dst = {0, 0, static_cast<std::int32_t>(dw), static_cast<std::int32_t>(dh)};
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0 || mode == FitMode::Stretch) return r;
    const std::uint64_t lhs = static_cast<std::uint64_t>(sw) * dh, rhs = static_cast<std::uint64_t>(dw) * sh;   // src aspect vs dst aspect
    if (mode == FitMode::Fit) {
        if (lhs >= rhs) {       // source is wider: full width, letterbox bars top/bottom
            const std::int32_t h = static_cast<std::int32_t>((static_cast<std::uint64_t>(sh) * dw) / sw);
            r.dst = {0, (static_cast<std::int32_t>(dh) - h) / 2, static_cast<std::int32_t>(dw), h};
        } else {
            const std::int32_t w = static_cast<std::int32_t>((static_cast<std::uint64_t>(sw) * dh) / sh);
            r.dst = {(static_cast<std::int32_t>(dw) - w) / 2, 0, w, static_cast<std::int32_t>(dh)};
        }
    } else {                    // Crop: trim the source so it fills the destination
        if (lhs >= rhs) {       // source wider: trim left/right
            const std::int32_t w = static_cast<std::int32_t>((static_cast<std::uint64_t>(dw) * sh) / dh);
            r.src = {(static_cast<std::int32_t>(sw) - w) / 2, 0, w, static_cast<std::int32_t>(sh)};
        } else {
            const std::int32_t h = static_cast<std::int32_t>((static_cast<std::uint64_t>(dh) * sw) / dw);
            r.src = {0, (static_cast<std::int32_t>(sh) - h) / 2, static_cast<std::int32_t>(sw), h};
        }
    }
    return r;
}

AdmissionDecision admitSources(Tier tier, const AdmissionRequest& q) {
    AdmissionDecision d;
    if (tier == Tier::Base) {
        if (q.videos) d.reasons.push_back("video needs STANDARD tier or higher");
        if (q.cameras) {
            if (q.baseCameraQualified) d.camerasAdmitted = 1;
            else d.reasons.push_back("BASE camera (320x240 at 10 FPS) is not admitted until it passes the benchmark on this machine");
            if (q.cameras > 1) d.reasons.push_back("only one camera is admitted");
        }
        if (q.gifs) {
            if (q.baseGifQualified) d.gifsAdmitted = 1;
            else d.reasons.push_back("BASE lightweight GIF is not admitted until it passes the benchmark on this machine");
            if (q.gifs > 1) d.reasons.push_back("only one GIF is admitted on BASE");
        }
        return d;
    }
    d.videosAdmitted = std::min<std::uint32_t>(q.videos, 1);
    d.camerasAdmitted = std::min<std::uint32_t>(q.cameras, 1);
    d.gifsAdmitted = q.gifs;
    if (q.videos > 1) d.reasons.push_back("only one video source is admitted");
    if (q.cameras > 1) d.reasons.push_back("only one camera is admitted");
    return d;
}

}  // namespace ss::media
