// compose limits (spec sections 7 and 13). Bounds are checked before any allocation.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ss::compose {

struct Limits {
    static constexpr std::uint32_t kMaxDimension = 4096;              // any buffer edge
    static constexpr std::uint64_t kMaxBufferBytes = 64ull << 20;     // per decoded image (section 13)
    static constexpr std::size_t kMaxMediaLayers = 4;
    static constexpr std::size_t kMaxActiveText = 4;                  // simultaneously published text items
    static constexpr std::size_t kMaxOverlayLines = 4;
    static constexpr std::size_t kMaxOverlayLineChars = 48;
    static constexpr std::size_t kMaxFramePool = 8;
    static constexpr std::size_t kMaxTextCodePoints = 4096;           // same as model::Limits::kMaxTextCodePoints
    // camera treatments
    static constexpr int kPosterizeMin = 2, kPosterizeMax = 16;
    static constexpr int kTrailMin = 2, kTrailMax = 8;
    static constexpr std::int64_t kMaxFreezeMs = 500;                 // REQ-S11
    // media
    static constexpr std::int64_t kMinGifDelayMs = 20;
    static constexpr std::size_t kMaxGifFrames = 1000;
    static constexpr double kGifSpeedMin = 0.25, kGifSpeedMax = 4.0;
    static constexpr double kVideoSpeedMin = 0.25, kVideoSpeedMax = 2.0;
    // text timing
    static constexpr double kMaxTextDurationSec = 3600.0, kMinScrollSec = 2.0, kMaxScrollSec = 120.0,
                            kDefaultScrollSec = 8.0;
    // adaptive internal render scale ladder (percent), index = ladder step
    static constexpr int kScaleLadderSize = 4;
    static constexpr int kScaleLadderPct[kScaleLadderSize] = {100, 75, 50, 35};
    static constexpr float kMinScale = 0.25f, kMaxScale = 1.0f;
};

}  // namespace ss::compose
