#include "ss/record/profile.hpp"
#include <algorithm>
#include <sstream>
#include "ss/base/sha256.hpp"
#include "ss/base/utf8.hpp"

namespace ss::record {
namespace {
Error bad(std::string d) { return makeError(ErrorCategory::InvalidArgument, std::move(d), Recoverability::OperatorAction); }

struct KnownEncoder { const char* name; VideoCodec codec; bool hardware; };
// Preference order for H.264: hardware first, then the BSD-licensed libopenh264.
const KnownEncoder kKnown[] = {
    {"h264_nvenc", VideoCodec::H264, true},       {"h264_qsv", VideoCodec::H264, true},
    {"h264_amf", VideoCodec::H264, true},         {"h264_videotoolbox", VideoCodec::H264, true},
    {"h264_mf", VideoCodec::H264, true},          {"h264_vaapi", VideoCodec::H264, true},
    {"libopenh264", VideoCodec::H264, false},     {"mjpeg", VideoCodec::Mjpeg, false},
};
}  // namespace

Result<RecordProfile> RecordProfile::create(const RecordProfileSpec& s) {
    if (s.audio.rateHz != 44100 && s.audio.rateHz != 48000) return bad("audio rate must be 44100 or 48000 Hz");
    if (s.audio.channels < 1 || s.audio.channels > 2) return bad("audio must be mono or stereo");
    if (s.segmentNs < Limits::kMinSegmentNs || s.segmentNs > Limits::kMaxSegmentNs) return bad("segment length must be 10 s to 60 min");
    if (s.outputDir.empty()) return bad("output folder is not set");
    if (s.showName.size() > 256 || !utf8::isValid(s.showName)) return bad("show name is too long or not valid UTF-8");
    if (s.audioDelayCompensationNs < 0 || s.audioDelayCompensationNs > secToNs(5)) return bad("audio delay compensation must be 0 to 5 s");
    if (s.segmentSizeBudgetBytes != 0) {
        const std::uint64_t maxB = s.kind == ProfileKind::AudioOnlyWav ? Limits::kWavSegmentMaxBytes : (64ull << 30);
        if (s.segmentSizeBudgetBytes < (16ull << 20) || s.segmentSizeBudgetBytes > maxB) return bad("segment size budget is outside the allowed range");
    }
    if (s.kind == ProfileKind::VideoMatroska) {
        const auto& v = s.video;
        if (v.width < 160 || v.height < 120 || v.width > 4096 || v.height > 4096 || (v.width & 1) || (v.height & 1))
            return bad("video size must be even and within 160x120 .. 4096x4096");
        if (v.fpsNum == 0 || v.fpsDen == 0) return bad("frame rate is invalid");
        const double fps = static_cast<double>(v.fpsNum) / v.fpsDen;
        if (fps < 5.0 || fps > 60.0) return bad("frame rate must be 5 to 60 FPS");
        if (v.h264BitrateKbps < 500 || v.h264BitrateKbps > 100000) return bad("H.264 bitrate must be 500 to 100000 kb/s");
        if (v.mjpegQuality < 2 || v.mjpegQuality > 31) return bad("MJPEG quality must be 2 to 31");
    }
    RecordProfile p;
    p.spec_ = std::make_shared<const RecordProfileSpec>(s);
    std::ostringstream o;
    o << int(s.kind) << '|' << s.audio.rateHz << '|' << s.audio.channels << '|' << s.video.width << 'x' << s.video.height << '|'
      << s.video.fpsNum << '/' << s.video.fpsDen << '|' << int(s.video.input) << '|' << s.video.h264BitrateKbps << '|' << s.video.mjpegQuality
      << '|' << int(s.codecPolicy) << '|' << s.segmentNs << '|' << p.effectiveSizeBudget() << '|' << s.audioDelayCompensationNs;
    p.hash_ = Sha256::hashHex(o.str());
    return p;
}

std::uint64_t RecordProfile::effectiveSizeBudget() const {
    if (spec_->segmentSizeBudgetBytes) return spec_->segmentSizeBudgetBytes;
    return spec_->kind == ProfileKind::AudioOnlyWav ? Limits::kDefaultAudioSizeBudget : Limits::kDefaultVideoSizeBudget;
}

bool isExcludedGplEncoder(const std::string& n) { return n == "libx264" || n == "libx264rgb" || n == "libx265"; }
bool isEligibleEncoder(const std::string& n) {
    return std::any_of(std::begin(kKnown), std::end(kKnown), [&](const KnownEncoder& k) { return n == k.name; });
}

std::vector<EncoderInfo> parseFfmpegEncoders(const std::string& text) {
    std::vector<EncoderInfo> out;
    std::istringstream in(text);
    std::string line;
    bool body = false;
    while (std::getline(in, line)) {
        if (!body) { if (line.find("------") != std::string::npos) body = true; continue; }
        std::size_t i = line.find_first_not_of(' ');
        if (i == std::string::npos) continue;
        std::istringstream ls(line.substr(i));
        std::string flags, name;
        if (!(ls >> flags >> name) || flags.size() != 6 || flags[0] != 'V') continue;
        for (const auto& k : kKnown)
            if (name == k.name) {
                EncoderInfo e; e.name = name; e.codec = k.codec; e.hardware = k.hardware; e.listed = true; e.verified = false;
                out.push_back(e);
            }
    }
    return out;
}

Result<EncoderChoice> selectVideoEncoder(const ProbeResults& probe, VideoCodecPolicy policy) {
    auto find = [&](const char* name) -> const EncoderInfo* {
        for (const auto& e : probe.encoders) if (e.name == name && e.usable() && isEligibleEncoder(e.name) && !isExcludedGplEncoder(e.name)) return &e;
        return nullptr;
    };
    if (!probe.ffmpegPresent)
        return makeError(ErrorCategory::EncoderUnavailable,
                         "FFmpeg was not found. Install FFmpeg or choose audio-only recording.", Recoverability::OperatorAction);
    if (policy != VideoCodecPolicy::MjpegOnly) {
        for (const auto& k : kKnown) {
            if (k.codec != VideoCodec::H264) continue;
            if (const EncoderInfo* e = find(k.name)) {
                EncoderChoice c; c.name = e->name; c.codec = VideoCodec::H264; c.hardware = e->hardware;
                return c;
            }
        }
        if (policy == VideoCodecPolicy::H264Only)
            return makeError(ErrorCategory::EncoderUnavailable,
                             "No verified H.264 encoder is available (hardware encoders and libopenh264 were checked; StageSignal never assumes x264). "
                             "Allow the MJPEG fallback or record audio-only.", Recoverability::OperatorAction);
    }
    if (const EncoderInfo* m = find("mjpeg")) {
        EncoderChoice c; c.name = m->name; c.codec = VideoCodec::Mjpeg; c.hardware = false;
        c.fallbackFromH264 = policy != VideoCodecPolicy::MjpegOnly;
        if (c.fallbackFromH264)
            c.note = "No verified H.264 encoder: using MJPEG. Files will be much larger (roughly 5-10x) than H.264.";
        return c;
    }
    return makeError(ErrorCategory::EncoderUnavailable,
                     "No usable video encoder was found. Record audio-only, or install an FFmpeg build with a hardware H.264 encoder or MJPEG.",
                     Recoverability::OperatorAction);
}

}  // namespace ss::record
