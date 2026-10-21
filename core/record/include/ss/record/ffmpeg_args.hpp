// StageSignal record: building the argv for an EXTERNAL ffmpeg process (DEC-12: ffmpeg is an optional separate
// executable, never linked or bundled). Output is always a std::vector<std::string> that the app hands to
// CreateProcess/posix_spawn directly: there is no shell string anywhere, so no quoting or injection surface.
//
// Verified against ffmpeg 6.1 and the ffmpeg-protocols/formats documentation (see docs/specs/record.md):
//   * raw video:  -f rawvideo -pix_fmt rgba|bgra -video_size WxH -framerate N/D -i <endpoint>
//   * raw audio:  -f f32le -ar R -ac C -i <endpoint>
//   * `pipe:N` reads file descriptor N (POSIX). On Windows pass a named-pipe path (\\.\pipe\name).
//   * -itsoffset applies to the NEXT -i and DELAYS that input by a positive value.
//   * segment muxer: -f segment -segment_format matroska -segment_time S -reset_timestamps 1
//     -segment_start_number 1 -segment_list <csv> -segment_list_type csv -segment_list_flags +live
//     with -force_key_frames "expr:gte(t,n_forced*S)" so cuts land exactly on the boundaries.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/record/profile.hpp"

namespace ss::record::ff {

struct RawVideoInput {
    std::string endpoint;                       // "pipe:3" | named pipe | absolute FIFO path
    std::uint32_t width = 0, height = 0, fpsNum = 30, fpsDen = 1;
    PixelFormat pixel = PixelFormat::Rgba;
};
struct PcmInput {
    std::string endpoint;
    std::uint32_t rateHz = 48000, channels = 2;  // interleaved float32 little-endian (the audio bus format)
};

// Host-clock origin of each stream's first delivered sample/frame, plus the known processing delay of the audio the
// recorder receives. All values come from the caller (capture/dynamics owners); nothing is guessed here.
struct InputSync {
    Ns videoStartHostNs = 0;                    // host time of video frame 0 (frame N is at start + N/fps; caller keeps CFR)
    Ns audioStartHostNs = 0;                    // host time at which capture of audio input sample 0 began
    Ns audioDelayCompNs = 0;                    // lookahead + filter group delay between capture and the delivered samples
};
struct InputOffsets { Ns video = 0, audio = 0; };   // both >= 0, one of them 0; feed to -itsoffset

// Audio output stream time of delivered sample n is  audioStart - delay + n/rate  (processing delays the content,
// so the content that was captured at time T arrives n = (T - audioStart + delay) * rate samples in). Relative to the
// video origin the audio stream therefore starts at (audioStart - delay) - videoStart. Negative values are shifted so
// that every input offset is >= 0 (muxers dislike negative start times).
InputOffsets computeInputOffsets(const InputSync& s);

// Accepts only local media endpoints: "pipe:<digits>", \\.\pipe\<name>, or an absolute path. Rejects URL schemes
// ("://"), anything starting with '-' (option injection), control characters and empty strings.
Status checkEndpoint(const std::string& endpoint);
std::string secondsArg(Ns ns);                  // decimal seconds with microsecond resolution, e.g. "0.250000"

struct VideoEncodeSpec {
    std::string encoder;                        // ffmpeg encoder name chosen by selectVideoEncoder
    VideoCodec codec = VideoCodec::H264;
    std::uint32_t bitrateKbps = 6000;
    int mjpegQuality = 5;
    std::uint32_t keyintSeconds = 2;            // H.264 only: -g = keyintSeconds * fps
    std::uint32_t fpsNum = 30, fpsDen = 1;
};
enum class AudioCodec : std::uint8_t { PcmS24le, Aac };
struct AudioEncodeSpec { AudioCodec codec = AudioCodec::PcmS24le; std::uint32_t bitrateKbps = 128; std::uint32_t rateHz = 48000, channels = 2; };

std::vector<std::string> globalArgs();          // -hide_banner -nostdin -nostats -loglevel warning
Status appendInputs(std::vector<std::string>& argv, const RawVideoInput& v, const PcmInput& a, const InputOffsets& off);
void appendVideoEncode(std::vector<std::string>& argv, const VideoEncodeSpec& e);
void appendAudioEncode(std::vector<std::string>& argv, const AudioEncodeSpec& e);
// "-force_key_frames expr:gte(t,n_forced*S)": keyframes at exact multiples of S seconds.
void appendForcedKeyframes(std::vector<std::string>& argv, std::uint32_t everySeconds);
// Rejects NUL/newline in any element (cannot be passed safely on every platform).
Status checkArgv(const std::vector<std::string>& argv);

}  // namespace ss::record::ff

namespace ss::record {

struct RecordCommandInput {
    std::string ffmpegPath = "ffmpeg";
    RecordProfile profile;
    EncoderChoice encoder;
    ff::RawVideoInput video;
    ff::PcmInput audio;
    ff::InputSync sync;                         // audioDelayCompNs is taken from profile.spec().audioDelayCompensationNs, not from here
    std::string sessionId;
    Ns sessionStartWallUnixNs = 0;
    explicit RecordCommandInput(RecordProfile p) : profile(std::move(p)) {}
};
struct RecordCommand {
    std::vector<std::string> argv;              // argv[0] is the executable
    std::string segmentListPath;                // csv written by the segment muxer (parse with parseSegmentListCsv)
    std::string segmentPattern;                 // path with %04d, first index 1
    Ns segmentDurationNs = 0;                   // effective cut interval (min of time and size-derived time)
    ff::InputOffsets offsets;
};

class FfmpegCommandBuilder {
public:
    // Video recording: segmented Matroska, 24-bit PCM audio, H.264 (hardware/libopenh264) or MJPEG fallback.
    static Result<RecordCommand> buildRecord(const RecordCommandInput& in);
};

}  // namespace ss::record
