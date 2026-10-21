// StageSignal media (MOD-07 rules): formats, tiers and the section 13 limits. Pure data, no I/O.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ss::media {

// Capability tier as named in spec sections 7/13. The perf module decides the tier; media only enforces limits.
enum class Tier : std::uint8_t { Base, Standard, Enhanced };

enum class Format : std::uint8_t { Unknown, Png, Jpeg, Bmp, Gif, Mp4, Mov, Mkv };

constexpr bool isStill(Format f) { return f == Format::Png || f == Format::Jpeg || f == Format::Bmp; }
constexpr bool isVideoContainer(Format f) { return f == Format::Mp4 || f == Format::Mov || f == Format::Mkv; }

inline const char* formatName(Format f) {
    switch (f) {
        case Format::Png: return "png";
        case Format::Jpeg: return "jpg";
        case Format::Bmp: return "bmp";
        case Format::Gif: return "gif";
        case Format::Mp4: return "mp4";
        case Format::Mov: return "mov";
        case Format::Mkv: return "mkv";
        case Format::Unknown: break;
    }
    return "unknown";
}

// Maps the `format` string stored in a MediaRef to a Format. This is a DECLARATION to be cross-checked
// against the sniffed bytes (REQ-S07); it never decides what a file is.
inline Format formatFromDeclared(const std::string& s) {
    if (s == "png") return Format::Png;
    if (s == "jpg" || s == "jpeg") return Format::Jpeg;
    if (s == "bmp") return Format::Bmp;
    if (s == "gif") return Format::Gif;
    if (s == "mp4") return Format::Mp4;
    if (s == "mov") return Format::Mov;
    if (s == "mkv") return Format::Mkv;
    return Format::Unknown;
}

// Spec section 13 limits (single source for the module and its tests).
struct Limits {
    static constexpr std::uint32_t kMaxStillDim = 4096;                    // width and height each
    static constexpr std::uint64_t kMaxStillDecodedBytes = 64ull << 20;    // 64 MiB per image
    static constexpr std::uint32_t kBaseStillW = 800, kBaseStillH = 600;   // BASE stills preprocessed to fit
    static constexpr std::uint64_t kBaseDecodedCacheBytes = 8ull << 20;    // BASE combined decoded cache
    static constexpr std::uint32_t kMaxGifFrames = 1000;
    static constexpr std::uint64_t kGifStreamCacheBytes = 64ull << 20;     // higher-tier streamed GIF cache
    static constexpr std::uint32_t kGifMinFramesInCache = 3;               // current + restore-previous + next
    static constexpr double kVideoSpeedMin = 0.25, kVideoSpeedMax = 2.0;
    static constexpr double kGifSpeedMin = 0.25, kGifSpeedMax = 4.0;
    static constexpr std::uint32_t kMinGifDelayMs = 20;                    // zero/sub-20 ms clamps to 20 ms
    static constexpr std::size_t kMaxTextCodePoints = 4096;
    static constexpr std::size_t kMaxMessages = 256;
    static constexpr std::uint32_t kMaxVideoDim = 4096;
    static constexpr std::uint32_t kBytesPerPixel = 4;                     // decoded RGBA8
};

}  // namespace ss::media
