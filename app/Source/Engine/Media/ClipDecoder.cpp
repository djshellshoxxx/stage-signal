#include "Engine/Media/ClipDecoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <juce_core/juce_core.h>
#include "nlohmann/json.hpp"

namespace ss::app {
namespace {
constexpr std::size_t kMaxGifBytes = 64u << 20;
constexpr Ns kFramePeriod = 40 * kNsPerMs;
double rate(const std::string& rational) {
    try {
        const auto slash = rational.find('/');
        const double numerator = std::stod(rational.substr(0, slash));
        const double denominator = slash == std::string::npos ? 1. : std::stod(rational.substr(slash + 1));
        return denominator > 0 ? numerator / denominator : 0.;
    } catch (...) { return 0.; }
}
}
ClipDecoder::ClipDecoder(IProcessSupervisor& processes, const IClock& clock, Probe probe)
    : processes_(processes), clock_(clock), probe_(std::move(probe)) {
    if (!probe_) probe_ = [this](const std::vector<std::string>& args, int timeout) { return runCaptured(processes_, args, timeout); };
}
ss::Status ClipDecoder::open(const std::string& path, const FfmpegProbe& ffmpeg, double speed, bool loop) {
    stop(); newest_.reset(); failed_ = ended_ = haveStart_ = false; nextFrame_ = 0;
    pending_.clear(); note_.clear(); frameBytes_ = 0;
    auto reject = [](const std::string& text) { return ss::Status(ss::makeError(ss::ErrorCategory::DecodeFailure, text)); };
    if (path.find('\0') != std::string::npos || !std::isfinite(speed)) return reject("Invalid media path or speed");
    const juce::File file(juce::String::fromUTF8(path.c_str()));
    if (!file.existsAsFile()) return reject("The media file is missing");
    auto input = file.createInputStream();
    if (!input) return reject("The media file could not be read");
    std::array<std::uint8_t, 512> header{};
    const int read = input->read(header.data(), static_cast<int>(header.size()));
    const auto sniffed = ss::media::sniff(header.data(), static_cast<std::size_t>(std::max(0, read)));
    const bool gif = sniffed.format == ss::media::Format::Gif;
    if (!gif && !ss::media::isVideoContainer(sniffed.format)) return reject("This file is not an allowed GIF or video container");
    if (!ffmpeg.found || ffmpeg.path.empty()) return reject("GIF/video playback needs a separately installed FFmpeg");
    auto speedStatus = ss::media::validateSpeed(gif ? ss::media::SpeedKind::Gif : ss::media::SpeedKind::Video, speed);
    if (!speedStatus.ok()) return speedStatus;
    std::uint32_t width = 0, height = 0;
    // GIF frame/disposal/delay metadata is validated before launching a decoder or allocating pixel frames.
    if (gif) {
        const auto byteCount = input->getTotalLength();
        if (byteCount <= 0 || static_cast<std::uint64_t>(byteCount) > kMaxGifBytes) return reject("GIF exceeds the bounded import budget");
        juce::MemoryBlock bytes(static_cast<std::size_t>(byteCount));
        input->setPosition(0);
        if (input->read(bytes.getData(), static_cast<int>(byteCount)) != byteCount) return reject("GIF is truncated");
        auto info = ss::media::parseGif(static_cast<const std::uint8_t*>(bytes.getData()), bytes.getSize());
        if (!info.ok()) return info.error();
        auto plan = ss::media::validateGif(info.value(), ss::media::Tier::Standard);
        if (!plan.ok()) return plan.error();
        width = plan.value().width; height = plan.value().height;
    } else {
        #if JUCE_WINDOWS
        const auto sibling = juce::File(ffmpeg.path).getSiblingFile("ffprobe.exe");
#else
        const auto sibling = juce::File(ffmpeg.path).getSiblingFile("ffprobe");
#endif
        const std::string ffprobe = sibling.existsAsFile() ? sibling.getFullPathName().toStdString() : "ffprobe";
        const auto captured = probe_({ffprobe, "-v", "error", "-protocol_whitelist", "file,pipe", "-select_streams", "v:0",
            "-show_entries", "stream=codec_name,width,height,avg_frame_rate:format=duration", "-of", "json", "-i", path}, 3000);
        if (!captured.started || !captured.exited || captured.code != 0 || captured.timedOut || captured.out.size() > (1u << 20))
            return reject("Video metadata probe failed or timed out; check the FFmpeg/ffprobe installation");
        try {
            const auto metadata = nlohmann::json::parse(captured.out);
            const auto& stream = metadata.at("streams").at(0);
            ss::media::VideoInfo info;
            info.container = sniffed.format; info.videoCodec = stream.at("codec_name").get<std::string>();
            info.width = stream.at("width").get<std::uint32_t>(); info.height = stream.at("height").get<std::uint32_t>();
            info.fps = rate(stream.at("avg_frame_rate").get<std::string>());
            info.durationSec = std::stod(metadata.at("format").at("duration").get<std::string>());
            auto plan = ss::media::validateVideo(info, ss::media::Tier::Standard);
            if (!plan.ok()) return plan.error();
            width = plan.value().width; height = plan.value().height;
        } catch (...) { return reject("Video metadata is incomplete or malformed"); }
    }
    std::uint32_t outW = 0, outH = 0;
    ss::media::fitWithin(width, height, 1280, 720, outW, outH);
    frameBytes_ = std::size_t(outW) * outH * 4;
    if (!frameBytes_ || frameBytes_ > (4u << 20)) return reject("Decoded frame exceeds the playback budget");
    working_ = PixelBuffer(outW, outH);
    pending_.reserve(frameBytes_ + (256u << 10));
    std::vector<std::string> argv{ffmpeg.path, "-hide_banner", "-loglevel", "error", "-nostdin", "-max_alloc", "67108864",
        "-threads", "1", "-filter_threads", "1", "-protocol_whitelist", "file,pipe"};
    if (gif) argv.insert(argv.end(), {"-ignore_loop", loop ? "1" : "0", "-min_delay", "2", "-default_delay", "2"});
    if (loop) argv.insert(argv.end(), {"-stream_loop", "-1"});
    argv.insert(argv.end(), {"-i", path, "-map", "0:v:0", "-an", "-sn", "-dn", "-vf",
        "setpts=PTS/" + std::to_string(speed) + ",fps=25,scale=" + std::to_string(outW) + ":" + std::to_string(outH),
        "-pix_fmt", "rgba", "-f", "rawvideo", "pipe:1"});
    child_ = processes_.create();
    if (!child_) return reject("Decoder process could not be created");
    auto status = child_->start(argv);
    if (!status.ok()) { child_.reset(); return status; }
    speed_ = speed; looping_ = loop; paused_ = false;
    openedAt_ = lastProgress_ = clock_.nowMonotonic();
    note_ = "Playing GIF/video from the media clock; imported media can flash.";
    return ss::okStatus();
}
void ClipDecoder::tick(Ns now) {
    if (!child_ || failed_ || ended_ || paused_) return;
    bool updated = false, drained = false;
    for (int budget = 0; budget < 16; ++budget) {
        if (pending_.size() >= frameBytes_) {
            if (!haveStart_) { haveStart_ = true; startedAt_ = now; }
            if (now - startedAt_ < static_cast<Ns>(nextFrame_) * kFramePeriod) break;
            std::memcpy(working_.rgba.data(), pending_.data(), frameBytes_);
            pending_.erase(0, frameBytes_); ++nextFrame_; updated = true;
            continue;
        }
        auto bytes = child_->readProgress();
        child_->readStderr();  // never expose decoder diagnostics containing input paths
        if (bytes.empty()) { drained = true; break; }
        if (bytes.size() > (256u << 10) || pending_.size() + bytes.size() > frameBytes_ + (256u << 10)) {
            note_ = "Decoder output exceeded its bounded queue"; failed_ = true; child_->kill(); break;
        }
        pending_ += bytes; lastProgress_ = now;
    }
    if (updated) {
        for (std::size_t i = 0; i < working_.rgba.size(); i += 4) {
            const auto alpha = working_.rgba[i + 3];
            for (std::size_t c = 0; c < 3; ++c) working_.rgba[i + c] = static_cast<std::uint8_t>((unsigned(working_.rgba[i + c]) * alpha + 127) / 255);
        }
        newest_ = std::make_shared<const PixelBuffer>(working_);
    }
    const bool running = child_->running();
    if (!running && drained && pending_.size() < frameBytes_) {
        if (child_->exitCode() != 0 || !haveStart_ || !pending_.empty()) { failed_ = true; note_ = "Media decoding failed or produced a truncated frame"; }
        else { ended_ = true; note_ = "Clip ended; the last frame is held."; }
    } else if ((!haveStart_ && now - openedAt_ > secToNs(3)) || (running && now - lastProgress_ > secToNs(5) && pending_.size() < frameBytes_)) {
        failed_ = true; note_ = "Media decoder stalled and was stopped"; child_->kill();
    }
}
void ClipDecoder::setPaused(bool paused, Ns now) {
    if (paused == paused_) return;
    if (paused) pausedAt_ = now;
    else {
        const Ns duration = std::max<Ns>(0, now - pausedAt_);
        if (haveStart_) startedAt_ += duration;
        openedAt_ += duration; lastProgress_ += duration;
    }
    paused_ = paused;
}
void ClipDecoder::stop() {
    if (child_) { child_->kill(); child_.reset(); }
    pending_.clear();
}
}  // namespace ss::app
