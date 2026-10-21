// StageSignal record (MOD-11 session logic): the immutable recording profile, encoder probe data and
// encoder selection (spec section 12 "Recording formats and lifecycle"). Pure logic, no I/O, no ffmpeg calls.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"

namespace ss::record {

struct Limits {
    static constexpr Ns kDefaultSegmentNs = 15ll * 60 * kNsPerSec;           // 15 min
    static constexpr Ns kMinSegmentNs = 10ll * kNsPerSec;
    static constexpr Ns kMaxSegmentNs = 60ll * 60 * kNsPerSec;
    static constexpr std::uint64_t kWarnFreeBytes = 2ull << 30;              // warn at 2 GiB free ...
    static constexpr Ns kWarnRemainingNs = 10ll * 60 * kNsPerSec;            // ... or ~10 min remaining
    static constexpr std::uint64_t kReserveMinBytes = 256ull << 20;          // graceful-stop reserve floor
    static constexpr Ns kReserveNs = 30ll * kNsPerSec;                       // ... or 30 s of output
    static constexpr Ns kMinStartRuntimeNs = 60ll * kNsPerSec;               // Preparing wants >= 60 s above the reserve
    static constexpr std::uint64_t kWavSegmentMaxBytes = 0xFFFFFFFFull - (1ull << 20);   // stay below the RIFF 4 GiB limit
    static constexpr std::uint64_t kDefaultAudioSizeBudget = 3500ull << 20;
    static constexpr std::uint64_t kDefaultVideoSizeBudget = 2048ull << 20;  // below the FAT32 4 GiB file limit
    static constexpr std::size_t kMaxMarkers = 10000;
    static constexpr std::size_t kMaxMarkerLabelCodePoints = 128;
    static constexpr Ns kFlushIntervalNs = 1ll * kNsPerSec;                  // durable flush + journal heartbeat period
};

enum class ProfileKind : std::uint8_t { AudioOnlyWav, VideoMatroska };
enum class VideoCodec : std::uint8_t { H264, Mjpeg };
enum class VideoCodecPolicy : std::uint8_t { PreferH264FallbackMjpeg, H264Only, MjpegOnly };
enum class PixelFormat : std::uint8_t { Rgba, Bgra };

struct AudioFormat { std::uint32_t rateHz = 48000, channels = 2; };          // output is 24-bit PCM
struct VideoFormat {
    std::uint32_t width = 1280, height = 720;
    std::uint32_t fpsNum = 30, fpsDen = 1;
    PixelFormat input = PixelFormat::Rgba;       // raw frames handed to the encoder through a pipe
    std::uint32_t h264BitrateKbps = 6000;
    int mjpegQuality = 5;                        // ffmpeg -q:v, 2 (best) .. 31
};

struct RecordProfileSpec {
    ProfileKind kind = ProfileKind::VideoMatroska;
    std::string showName = "show";               // sanitised again when building file names
    AudioFormat audio;
    VideoFormat video;                           // ignored for AudioOnlyWav
    VideoCodecPolicy codecPolicy = VideoCodecPolicy::PreferH264FallbackMjpeg;
    Ns segmentNs = Limits::kDefaultSegmentNs;
    std::uint64_t segmentSizeBudgetBytes = 0;    // 0 = kind default
    std::string outputDir;                       // opaque to this module; never copied into error text
    // Known processing delay between capture and the samples handed to the recorder (compressor/limiter lookahead,
    // filter group delay). Supplied by the dynamics owner; subtracted from audio PTS by the command builder.
    Ns audioDelayCompensationNs = 0;
};

// Immutable once created (REQ-R06: "immutable profile" is verified in Preparing). Cheap to copy.
class RecordProfile {
public:
    static Result<RecordProfile> create(const RecordProfileSpec& spec);
    const RecordProfileSpec& spec() const { return *spec_; }
    const std::string& hash() const { return hash_; }               // SHA-256 over the encoding-relevant fields
    std::uint64_t effectiveSizeBudget() const;
    bool hasVideo() const { return spec_->kind == ProfileKind::VideoMatroska; }
private:
    RecordProfile() = default;
    std::shared_ptr<const RecordProfileSpec> spec_;
    std::string hash_;
};

// ---- encoder probe data (the app runs ffmpeg; results are passed in as plain data) ----------------------------
struct EncoderInfo {
    std::string name;                // ffmpeg encoder name
    VideoCodec codec = VideoCodec::H264;
    bool hardware = false;
    bool listed = false;             // appears in `ffmpeg -encoders`
    bool verified = false;           // a short test encode with it succeeded on this machine
    bool usable() const { return listed && verified; }
};
struct ProbeResults {
    bool ffmpegPresent = false;
    bool segmentMuxer = true, matroskaMuxer = true, pcmS24le = true, flvMuxer = true, mpegtsMuxer = true, aacEncoder = true;
    std::string ffmpegVersion;       // informational
    std::vector<EncoderInfo> encoders;
};

// Turns the text of `ffmpeg -hide_banner -encoders` into EncoderInfo entries (listed=true, verified=false) for
// every encoder StageSignal knows how to drive. Unknown encoders and libx264/libx265 are never returned.
std::vector<EncoderInfo> parseFfmpegEncoders(const std::string& encodersOutput);
// True for encoders on the eligibility allowlist (hardware H.264 APIs, libopenh264, native mjpeg).
bool isEligibleEncoder(const std::string& name);
// True for GPL encoders that StageSignal never selects or assumes (libx264, libx264rgb, libx265).
bool isExcludedGplEncoder(const std::string& name);

struct EncoderChoice {
    std::string name;
    VideoCodec codec = VideoCodec::H264;
    bool hardware = false;
    bool fallbackFromH264 = false;   // MJPEG chosen because no eligible H.264 encoder was verified
    std::string note;                // operator-facing explanation when fallbackFromH264
};
Result<EncoderChoice> selectVideoEncoder(const ProbeResults& probe, VideoCodecPolicy policy);

}  // namespace ss::record
