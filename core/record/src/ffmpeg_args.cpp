#include "ss/record/ffmpeg_args.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include "ss/record/disk.hpp"
#include "ss/record/naming.hpp"
#include "ss/record/segment.hpp"

namespace ss::record::ff {
namespace {
Error bad(std::string d) { return makeError(ErrorCategory::InvalidArgument, std::move(d), Recoverability::OperatorAction); }
bool hasControl(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
}
}  // namespace

InputOffsets computeInputOffsets(const InputSync& s) {
    const Ns audioRel = (s.audioStartHostNs - s.audioDelayCompNs) - s.videoStartHostNs;
    const Ns shift = std::min<Ns>(0, audioRel);
    InputOffsets o;
    o.video = -shift;
    o.audio = audioRel - shift;
    return o;
}

Status checkEndpoint(const std::string& e) {
    if (e.empty() || hasControl(e)) return bad("media endpoint is empty or contains control characters");
    if (e[0] == '-') return bad("media endpoint must not start with '-'");
    if (e.find("://") != std::string::npos) return bad("media endpoint must be a local pipe, not a URL");
    if (e.rfind("pipe:", 0) == 0) {
        const std::string n = e.substr(5);
        if (n.empty() || !std::all_of(n.begin(), n.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            return bad("pipe endpoint must be pipe:<fd number>");
        return okStatus();
    }
    if (e.rfind("\\\\.\\pipe\\", 0) == 0) return e.size() > 9 ? okStatus() : Status(bad("named pipe has no name"));
    const bool posixAbs = e[0] == '/';
    const bool winDrive = e.size() > 3 && std::isalpha(static_cast<unsigned char>(e[0])) && e[1] == ':' && (e[2] == '\\' || e[2] == '/');
    if (posixAbs || winDrive) return okStatus();
    return bad("media endpoint must be pipe:<n>, a named pipe, or an absolute path");
}

std::string secondsArg(Ns ns) {
    const bool neg = ns < 0;
    const std::uint64_t us = static_cast<std::uint64_t>((neg ? -ns : ns) / 1000);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s%llu.%06llu", neg ? "-" : "", static_cast<unsigned long long>(us / 1000000), static_cast<unsigned long long>(us % 1000000));
    return buf;
}

std::vector<std::string> globalArgs() { return {"-hide_banner", "-nostdin", "-nostats", "-loglevel", "warning"}; }

Status appendInputs(std::vector<std::string>& a, const RawVideoInput& v, const PcmInput& p, const InputOffsets& off) {
    Status s = checkEndpoint(v.endpoint);
    if (!s.ok()) return s;
    s = checkEndpoint(p.endpoint);
    if (!s.ok()) return s;
    if (v.endpoint == p.endpoint) return bad("video and audio must use different endpoints");
    if (v.width == 0 || v.height == 0 || v.fpsNum == 0 || v.fpsDen == 0 || p.rateHz == 0 || p.channels == 0 || p.channels > 8)
        return bad("input format is incomplete");
    for (const char* x : {"-f", "rawvideo", "-pix_fmt"}) a.emplace_back(x);
    a.emplace_back(v.pixel == PixelFormat::Bgra ? "bgra" : "rgba");
    a.emplace_back("-video_size"); a.push_back(std::to_string(v.width) + "x" + std::to_string(v.height));
    a.emplace_back("-framerate"); a.push_back(std::to_string(v.fpsNum) + "/" + std::to_string(v.fpsDen));
    a.emplace_back("-thread_queue_size"); a.emplace_back("512");     // pipes: avoid "thread message queue blocking" stalls
    a.emplace_back("-itsoffset"); a.push_back(secondsArg(off.video));
    a.emplace_back("-i"); a.push_back(v.endpoint);
    a.emplace_back("-f"); a.emplace_back("f32le");
    a.emplace_back("-ar"); a.push_back(std::to_string(p.rateHz));
    a.emplace_back("-ac"); a.push_back(std::to_string(p.channels));
    a.emplace_back("-thread_queue_size"); a.emplace_back("512");
    a.emplace_back("-itsoffset"); a.push_back(secondsArg(off.audio));
    a.emplace_back("-i"); a.push_back(p.endpoint);
    a.emplace_back("-map"); a.emplace_back("0:v:0");
    a.emplace_back("-map"); a.emplace_back("1:a:0");
    return okStatus();
}

void appendVideoEncode(std::vector<std::string>& a, const VideoEncodeSpec& e) {
    a.emplace_back("-c:v"); a.push_back(e.encoder);
    if (e.codec == VideoCodec::Mjpeg) {
        a.emplace_back("-q:v"); a.push_back(std::to_string(e.mjpegQuality));
        a.emplace_back("-pix_fmt"); a.emplace_back("yuvj420p");
        return;
    }
    const std::string k = std::to_string(e.bitrateKbps) + "k";
    a.emplace_back("-pix_fmt"); a.emplace_back("yuv420p");
    a.emplace_back("-b:v"); a.push_back(k);
    a.emplace_back("-maxrate"); a.push_back(k);
    a.emplace_back("-bufsize"); a.push_back(std::to_string(e.bitrateKbps * 2) + "k");
    const double fps = static_cast<double>(e.fpsNum) / e.fpsDen;
    a.emplace_back("-g"); a.push_back(std::to_string(std::max<std::uint32_t>(1, static_cast<std::uint32_t>(fps * e.keyintSeconds + 0.5))));
}

void appendAudioEncode(std::vector<std::string>& a, const AudioEncodeSpec& e) {
    if (e.codec == AudioCodec::PcmS24le) {
        a.emplace_back("-c:a"); a.emplace_back("pcm_s24le");
    } else {
        a.emplace_back("-c:a"); a.emplace_back("aac");
        a.emplace_back("-b:a"); a.push_back(std::to_string(e.bitrateKbps) + "k");
    }
    a.emplace_back("-ar"); a.push_back(std::to_string(e.rateHz));
    a.emplace_back("-ac"); a.push_back(std::to_string(e.channels));
}

void appendForcedKeyframes(std::vector<std::string>& a, std::uint32_t every) {
    a.emplace_back("-force_key_frames");
    a.push_back("expr:gte(t,n_forced*" + std::to_string(every) + ")");
}

Status checkArgv(const std::vector<std::string>& argv) {
    if (argv.empty() || argv[0].empty()) return bad("command has no executable");
    for (const auto& s : argv)
        if (s.find('\0') != std::string::npos || s.find('\n') != std::string::npos || s.find('\r') != std::string::npos)
            return bad("command argument contains a control character");
    return okStatus();
}

}  // namespace ss::record::ff

namespace ss::record {

Result<RecordCommand> FfmpegCommandBuilder::buildRecord(const RecordCommandInput& in) {
    auto bad = [](std::string d) { return makeError(ErrorCategory::InvalidArgument, std::move(d), Recoverability::OperatorAction); };
    const auto& spec = in.profile.spec();
    if (!in.profile.hasVideo()) return bad("audio-only profiles are written by AudioOnlyRecorder, not ffmpeg");
    if (in.ffmpegPath.empty()) return bad("ffmpeg path is empty");
    if (!isEligibleEncoder(in.encoder.name) || isExcludedGplEncoder(in.encoder.name))
        return makeError(ErrorCategory::EncoderUnavailable, "encoder is not on the eligibility allowlist", Recoverability::OperatorAction);
    if ((in.encoder.codec == VideoCodec::Mjpeg) != (in.encoder.name == "mjpeg")) return bad("encoder name and codec disagree");
    if (!isValidSessionId(in.sessionId)) return bad("invalid session id");
    const auto& v = in.video;
    if (v.width != spec.video.width || v.height != spec.video.height || v.fpsNum != spec.video.fpsNum || v.fpsDen != spec.video.fpsDen)
        return bad("video input does not match the recording profile");
    if (in.audio.rateHz != spec.audio.rateHz || in.audio.channels != spec.audio.channels) return bad("audio input does not match the recording profile");
    const std::string& dir = spec.outputDir;
    const bool dirAbs = !dir.empty() && (dir[0] == '/' || dir.rfind("\\\\", 0) == 0 ||
                                         (dir.size() > 2 && std::isalpha(static_cast<unsigned char>(dir[0])) && dir[1] == ':' && (dir[2] == '\\' || dir[2] == '/')));
    if (!dirAbs || dir.find("://") != std::string::npos || std::any_of(dir.begin(), dir.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; }))
        return bad("output folder must be an absolute local path");

    RecordCommand cmd;
    ff::InputSync sync = in.sync;
    sync.audioDelayCompNs = spec.audioDelayCompensationNs;       // the immutable profile is the single source of truth
    cmd.offsets = ff::computeInputOffsets(sync);
    cmd.segmentDurationNs = videoSegmentDuration(spec.segmentNs, in.profile.effectiveSizeBudget(),
                                                 nominalRateFor(in.profile, in.encoder.codec).bytesPerSec * (1.0 + nominalRateFor(in.profile, in.encoder.codec).relUncertainty));
    const std::string sep = (dir.back() == '/' || dir.back() == '\\') ? "" : "/";
    std::string dirEsc;
    for (char c : dir) { dirEsc.push_back(c); if (c == '%') dirEsc.push_back('%'); }
    cmd.segmentPattern = dirEsc + sep + segmentPattern(in.sessionStartWallUnixNs, in.sessionId, spec.showName, "mkv");
    cmd.segmentListPath = dir + sep + segmentListFileName(in.sessionStartWallUnixNs, in.sessionId, spec.showName);

    auto& a = cmd.argv;
    a.push_back(in.ffmpegPath);
    for (auto& g : ff::globalArgs()) a.push_back(g);
    Status s = ff::appendInputs(a, v, in.audio, cmd.offsets);
    if (!s.ok()) return s.error();
    ff::VideoEncodeSpec ve;
    ve.encoder = in.encoder.name; ve.codec = in.encoder.codec; ve.bitrateKbps = spec.video.h264BitrateKbps;
    ve.mjpegQuality = spec.video.mjpegQuality; ve.fpsNum = spec.video.fpsNum; ve.fpsDen = spec.video.fpsDen;
    ff::appendVideoEncode(a, ve);
    if (in.encoder.codec == VideoCodec::H264) ff::appendForcedKeyframes(a, static_cast<std::uint32_t>(cmd.segmentDurationNs / kNsPerSec));
    ff::AudioEncodeSpec ae; ae.codec = ff::AudioCodec::PcmS24le; ae.rateHz = spec.audio.rateHz; ae.channels = spec.audio.channels;
    ff::appendAudioEncode(a, ae);
    for (const char* x : {"-f", "segment", "-segment_format", "matroska", "-segment_format_options", "cluster_time_limit=1000:flush_packets=1",
                          "-segment_time"}) a.emplace_back(x);
    a.push_back(std::to_string(cmd.segmentDurationNs / kNsPerSec));
    for (const char* x : {"-segment_start_number", "1", "-reset_timestamps", "1", "-segment_list"}) a.emplace_back(x);
    a.push_back(cmd.segmentListPath);
    for (const char* x : {"-segment_list_type", "csv", "-segment_list_flags", "+live"}) a.emplace_back(x);
    a.push_back(cmd.segmentPattern);
    s = ff::checkArgv(a);
    if (!s.ok()) return s.error();
    return cmd;
}

}  // namespace ss::record
