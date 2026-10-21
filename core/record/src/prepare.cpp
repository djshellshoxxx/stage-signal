#include "ss/record/prepare.hpp"
#include <cmath>

namespace ss::record {
namespace {
void add(PrepareReport& r, IssueCode c, bool blocking, std::string msg, std::string act) {
    r.issues.push_back({c, blocking, std::move(msg), std::move(act)});
}
}  // namespace

PrepareReport prepareChecks(const RecordProfile& profile, const PrepareInputs& in) {
    PrepareReport r;
    const auto& s = profile.spec();
    r.profileHash = profile.hash();
    r.nominal = nominalRate(profile);
    // Encoder selection first (video only) so space planning uses the codec that will really run.
    Result<EncoderChoice> enc = makeError(ErrorCategory::EncoderUnavailable, "not selected");
    if (profile.hasVideo() && in.encoders.ffmpegPresent) {
        enc = selectVideoEncoder(in.encoders, s.codecPolicy);
        if (enc.ok()) r.nominal = nominalRateFor(profile, enc.value().codec);
    }

    // 1. Output folder
    if (!in.path.exists || !in.path.isDirectory)
        add(r, IssueCode::PathMissing, true, "The recording folder does not exist.", "Choose an existing folder in the recording settings.");
    else if (!in.path.writable)
        add(r, IssueCode::PathNotWritable, true, "The recording folder is not writable.", "Pick a folder you can write to, or fix its permissions.");

    // 2. Free space: reserve + at least 60 s at the pessimistic rate
    RateEstimate est; est.bytesPerSec = r.nominal.bytesPerSec; est.relUncertainty = r.nominal.relUncertainty;
    const std::uint64_t reserve = DiskMonitor::reserveFor(est);
    const double minRun = est.upper() * nsToSec(Limits::kMinStartRuntimeNs);
    r.requiredFreeBytes = reserve + static_cast<std::uint64_t>(std::ceil(minRun));
    if (!in.disk.valid) {
        add(r, IssueCode::DiskUnknown, true, "Free disk space could not be determined.", "Check that the recording drive is connected, then try again.");
    } else if (in.disk.freeBytes < r.requiredFreeBytes) {
        add(r, IssueCode::DiskTooLow, true, "Not enough free disk space to start recording safely.",
            "Free up disk space or choose another drive (needs about " + std::to_string(r.requiredFreeBytes >> 20) + " MiB free).");
    } else if (in.disk.freeBytes <= Limits::kWarnFreeBytes) {
        add(r, IssueCode::DiskLowWarning, false, "Free disk space is under 2 GiB; the recording may stop early.", "Free up space for a long recording.");
    }

    // 3. Audio source (always required: both profiles record audio)
    if (!in.sources.audioConfigured || !in.sources.audioDeviceOk)
        add(r, IssueCode::AudioSourceMissing, true, "The selected audio input is not available.", "Reconnect the audio device or choose another input.");
    else if (in.sources.audioRateHz != s.audio.rateHz || in.sources.audioChannels != s.audio.channels)
        add(r, IssueCode::AudioFormatMismatch, true, "The audio input format does not match the recording profile.",
            "Use " + std::to_string(s.audio.rateHz) + " Hz, " + std::to_string(s.audio.channels) + " channel(s), or change the profile.");

    // 4. Video-only checks. Audio-only needs no renderer, no encoder, no ffmpeg (REQ-R05).
    if (profile.hasVideo()) {
        if (!in.sources.videoConfigured || !in.sources.videoSourceOk)
            add(r, IssueCode::VideoSourceMissing, true, "The program video output is not available.", "Start the visual output, or record audio-only.");
        else if (in.sources.videoWidth != s.video.width || in.sources.videoHeight != s.video.height ||
                 (in.sources.videoRateKnown && std::fabs(in.sources.videoFps - static_cast<double>(s.video.fpsNum) / s.video.fpsDen) > 0.5))
            add(r, IssueCode::VideoFormatMismatch, true, "The program video size or frame rate does not match the recording profile.",
                "Match the profile to the output (" + std::to_string(s.video.width) + "x" + std::to_string(s.video.height) + "), or record audio-only.");
        if (!in.encoders.ffmpegPresent) {
            add(r, IssueCode::FfmpegMissing, true, "FFmpeg was not found, so video cannot be recorded.", "Install FFmpeg, or record audio-only.");
        } else {
            if (!in.encoders.segmentMuxer || !in.encoders.matroskaMuxer || !in.encoders.pcmS24le)
                add(r, IssueCode::MuxerMissing, true, "This FFmpeg build lacks the Matroska/segment muxer or 24-bit PCM.", "Use a standard FFmpeg build, or record audio-only.");
            if (!enc.ok()) add(r, IssueCode::EncoderUnavailable, true, enc.error().detail, "Record audio-only now, or install an FFmpeg build with a hardware H.264 encoder.");
            else {
                r.encoder = enc.value();
                if (enc.value().fallbackFromH264) add(r, IssueCode::EncoderFallback, false, enc.value().note, "Expect large files; consider shorter segments.");
            }
        }
    }

    r.ok = true;
    for (const auto& i : r.issues) if (i.blocking) r.ok = false;
    if (!r.ok) r.encoder.reset();
    return r;
}

}  // namespace ss::record
