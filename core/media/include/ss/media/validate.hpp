// StageSignal media: limit validators (REQ-S07, section 13). Every function here runs on header-level
// numbers BEFORE any pixel buffer is allocated; a rejected input never reaches a decoder.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/media/format.hpp"
#include "ss/media/probe.hpp"

namespace ss::media {

// Decoded-cache accounting. Reservations are checked and recorded before the buffer is allocated.
class DecodedCacheBudget {
public:
    explicit DecodedCacheBudget(std::uint64_t capBytes) : cap_(capBytes) {}
    // BASE: 8 MiB combined. STANDARD+: no aggregate cap here (per-image 64 MiB applies); perf admission owns memory.
    static DecodedCacheBudget forTier(Tier t) {
        return DecodedCacheBudget(t == Tier::Base ? Limits::kBaseDecodedCacheBytes : ~0ull);
    }
    bool tryReserve(std::uint64_t bytes) {
        if (bytes > cap_ - used_) return false;
        used_ += bytes; return true;
    }
    void release(std::uint64_t bytes) { used_ = bytes > used_ ? 0 : used_ - bytes; }
    std::uint64_t used() const { return used_; }
    std::uint64_t cap() const { return cap_; }
    std::uint64_t remaining() const { return cap_ - used_; }
private:
    std::uint64_t cap_, used_ = 0;
};

// ---- stills ------------------------------------------------------------------------------------------------
struct StillPlan {
    std::uint32_t srcWidth = 0, srcHeight = 0;
    std::uint32_t targetWidth = 0, targetHeight = 0;     // what the live cache will hold
    std::uint64_t decodedBytes = 0;                      // RGBA8 bytes of the target
    bool needsDownscale = false;                         // BASE: preprocess OFF the live path to fit 800x600
    std::vector<std::string> warnings;
};
// `cache` (optional) is consulted for the BASE combined-cache rule but NOT modified; the caller reserves
// plan.decodedBytes on success.
Result<StillPlan> validateStill(std::uint32_t width, std::uint32_t height, Tier tier,
                                const DecodedCacheBudget* cache = nullptr);

// Aspect-preserving size that fits inside maxW x maxH (never upscales).
void fitWithin(std::uint32_t w, std::uint32_t h, std::uint32_t maxW, std::uint32_t maxH,
               std::uint32_t& outW, std::uint32_t& outH);

// ---- GIF ---------------------------------------------------------------------------------------------------
struct GifTiming {
    std::vector<std::uint32_t> delaysMs;     // after clamping, every entry >= 20
    std::uint32_t clampedCount = 0;
    std::vector<std::string> warnings;
};
// Zero or sub-20 ms delays clamp to 20 ms and produce one warning per call (with the count).
GifTiming normalizeGifDelays(const std::vector<std::uint32_t>& rawDelaysMs);

struct GifPlan {
    std::uint32_t width = 0, height = 0, frameCount = 0;
    std::uint64_t frameBytes = 0;                // one decoded RGBA canvas
    std::uint32_t cacheFrames = 0;               // frames the streamed decode may hold at once
    std::uint64_t cacheBytes = 0;
    GifTiming timing;
    int loopCount = -1;
};
Result<GifPlan> validateGif(const GifInfo& info, Tier tier, const DecodedCacheBudget* cache = nullptr);

// Plays frames from the MEDIA clock, never from a render tick count (section 13).
class GifTimeline {
public:
    GifTimeline() = default;
    GifTimeline(const std::vector<std::uint32_t>& delaysMs, int loopCount);
    std::uint32_t frameCount() const { return static_cast<std::uint32_t>(cumMs_.size()); }
    Ns cycleNs() const { return cycleNs_; }
    // elapsedWallNs = time since the clip started; speed already validated (0.25-4).
    // Returns the frame index, or the last frame once a finite loop count is exhausted (*finished set).
    std::uint32_t frameAt(Ns elapsedWallNs, double speed, bool* finished = nullptr) const;
private:
    std::vector<std::uint64_t> cumMs_;       // cumulative end time of each frame
    Ns cycleNs_ = 0;
    int loops_ = -1;
};

// ---- video / speed / text ----------------------------------------------------------------------------------
enum class SpeedKind : std::uint8_t { Video, Gif };
Status validateSpeed(SpeedKind kind, double speed);                 // rejects NaN/inf/out-of-range
double clampSpeed(SpeedKind kind, double speed);                    // for UI sliders; NaN -> 1.0

// Metadata a decoder worker reports after probing a video container (the container itself is only sniffed here).
struct VideoInfo {
    Format container = Format::Unknown;
    std::string videoCodec;                  // lower case, e.g. "h264", "mjpeg"
    std::uint32_t width = 0, height = 0;
    double fps = 0, durationSec = 0;
};
struct VideoPlan { std::uint32_t width = 0, height = 0; std::uint64_t frameBytes = 0; std::vector<std::string> warnings; };
Result<VideoPlan> validateVideo(const VideoInfo& info, Tier tier);

Status validateText(const std::string& utf8);                       // valid UTF-8, <= 4096 code points, no NUL
Status validateMessageCount(std::size_t count);                     // <= 256 per show

// Position inside a clip for the media clock. `loop` false => *ended set and position pinned to the end.
Ns loopPosition(Ns elapsedWallNs, double speed, Ns durationNs, bool loop, bool* ended = nullptr);

// ---- crop / fit (section 13 composition rules) ---------------------------------------------------------------
struct CropRect { float x = 0, y = 0, w = 1, h = 1; };              // normalised source rectangle
Status validateCrop(const CropRect& c);
enum class FitMode : std::uint8_t { Fit, Crop, Stretch };
struct PixelRect { std::int32_t x = 0, y = 0, w = 0, h = 0; };
struct FitResult { PixelRect src, dst; };
// Fit: letterbox the whole source inside dst. Crop: fill dst, trimming the source. Stretch: ignore aspect.
FitResult computeFit(std::uint32_t srcW, std::uint32_t srcH, std::uint32_t dstW, std::uint32_t dstH, FitMode mode);

// ---- admission ---------------------------------------------------------------------------------------------
struct AdmissionRequest {
    std::uint32_t videos = 0, cameras = 0, gifs = 0;
    bool baseCameraQualified = false;        // BASE 320x240/10 FPS spike benchmarked on this machine
    bool baseGifQualified = false;           // BASE lightweight-GIF benchmark passed
};
struct AdmissionDecision {
    std::uint32_t videosAdmitted = 0, camerasAdmitted = 0, gifsAdmitted = 0;
    std::vector<std::string> reasons;        // one human-readable line per refusal
    bool all(const AdmissionRequest& r) const {
        return videosAdmitted == r.videos && camerasAdmitted == r.cameras && gifsAdmitted == r.gifs;
    }
};
AdmissionDecision admitSources(Tier tier, const AdmissionRequest& req);

}  // namespace ss::media
