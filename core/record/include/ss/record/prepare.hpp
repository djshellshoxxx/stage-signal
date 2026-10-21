// StageSignal record: the Preparing checks (REQ-R06: "Codec unavailable/full disk/capture invalid produces
// actionable start failure before claiming Recording"). Every probe result arrives as plain data; this
// module never touches the file system, devices or ffmpeg.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "ss/record/disk.hpp"
#include "ss/record/profile.hpp"

namespace ss::record {

struct PathProbe {                    // the app tried to create+delete a temp file in the output folder
    bool exists = false, isDirectory = false, writable = false;
};
struct DiskProbe { bool valid = false; std::uint64_t freeBytes = 0; };
struct SourceProbe {                  // what the capture/render layers report right now
    bool audioConfigured = false, audioDeviceOk = false;
    std::uint32_t audioRateHz = 0, audioChannels = 0;
    bool videoConfigured = false, videoSourceOk = false;
    std::uint32_t videoWidth = 0, videoHeight = 0;
    bool videoRateKnown = false;
    double videoFps = 0;
};
struct PrepareInputs {
    PathProbe path;
    DiskProbe disk;
    ProbeResults encoders;            // ignored for audio-only profiles
    SourceProbe sources;
};

enum class IssueCode : std::uint8_t {
    PathMissing, PathNotWritable, DiskUnknown, DiskTooLow, DiskLowWarning,
    FfmpegMissing, EncoderUnavailable, EncoderFallback, MuxerMissing,
    AudioSourceMissing, AudioFormatMismatch, VideoSourceMissing, VideoFormatMismatch, NotPreparing
};
struct PrepareIssue {
    IssueCode code;
    bool blocking;                    // blocking issues prevent Recording; the others are warnings
    std::string message;              // what is wrong
    std::string action;               // what the operator can do about it (actionable, no paths)
};
struct PrepareReport {
    bool ok = false;
    std::vector<PrepareIssue> issues;
    std::optional<EncoderChoice> encoder;       // set for video profiles when ok
    std::uint64_t requiredFreeBytes = 0;        // reserve + >= 60 s at the pessimistic nominal rate
    NominalRate nominal;
    std::string profileHash;                    // the immutable profile that was verified
};

PrepareReport prepareChecks(const RecordProfile& profile, const PrepareInputs& in);

}  // namespace ss::record
