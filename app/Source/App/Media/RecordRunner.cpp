#include "App/Media/RecordRunner.h"
#include <algorithm>
#include <juce_core/juce_core.h>
#include "ss/record/ffmpeg_args.hpp"
#include "ss/record/naming.hpp"
#include "ss/record/prepare.hpp"

namespace ss::app {
namespace rec = ss::record;

namespace {
std::string firstIssue(const rec::PrepareReport& r) {
    for (const auto& i : r.issues)
        if (i.blocking) return i.message + (i.action.empty() ? "" : ". " + i.action);
    return "Recording cannot start";
}
std::uint32_t evenDown(std::uint32_t v) { return std::max<std::uint32_t>(2, v & ~1u); }
}  // namespace

std::uint64_t RecordRunner::freeBytes() const {
    return static_cast<std::uint64_t>(std::max<juce::int64>(0, juce::File(juce::String::fromUTF8(dir_.c_str())).getBytesFreeOnVolume()));
}

void RecordRunner::fail(const std::string& why) {
    note_ = why;
    if (rec_ && rec_->running()) rec_->stop(false);
    if (proc_) { proc_->kill(); proc_.reset(); }
    session_.onWriterFault(ss::makeError(ss::ErrorCategory::InternalFault, why));
    journal_.close();
    phase_ = Phase::Failed;
}

void RecordRunner::begin(const MediaConfig& cfg, const FfmpegProbe* probe, std::uint32_t rate, std::uint32_t channels,
                         const ss::PixelBuffer* frame) {
    if (active()) return;
    session_.reset();
    rec_.reset(); fs_.reset(); proc_.reset(); profile_.reset();
    note_.clear(); videoNote_.clear(); stderrTail_.clear();
    videoDropped_ = audioDropped_ = videoFrames_ = 0; openedSegs_ = closedSegs_ = 0; fps_ = std::clamp<std::uint32_t>(cfg.recordFps, 5, 60);
    rate_ = rate; ch_ = channels;
    wallStart_ = clock_.wallUnixNs();
    show_ = cfg.showName.empty() ? "show" : cfg.showName;

    juce::File dirF = cfg.outputDir.empty()
        ? juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("StageSignal Recordings")
        : juce::File(juce::String::fromUTF8(cfg.outputDir.c_str()));
    dir_ = dirF.getFullPathName().toStdString();
    ss::Rng rng(static_cast<std::uint64_t>(clock_.wallUnixNs()) ^ 0x5eedULL);
    sessionId_ = rec::makeSessionId(rng);

    rec::PrepareInputs in;
    dirF.createDirectory();
    in.path.exists = dirF.exists();
    in.path.isDirectory = dirF.isDirectory();
    if (in.path.isDirectory) {
        auto probeFile = dirF.getChildFile(".ss_write_test");
        in.path.writable = probeFile.replaceWithText("x");
        probeFile.deleteFile();
    }
    const auto fb = freeBytes();
    in.disk.valid = fb > 0;
    in.disk.freeBytes = fb;
    in.sources.audioConfigured = rate > 0 && channels > 0;
    in.sources.audioDeviceOk = in.sources.audioConfigured;
    in.sources.audioRateHz = rate; in.sources.audioChannels = channels;

    rec::RecordProfileSpec spec;
    spec.kind = rec::ProfileKind::AudioOnlyWav;
    spec.showName = show_;
    spec.audio = {rate ? rate : 48000, channels ? channels : 2};
    spec.outputDir = dir_;
    auto prof = rec::RecordProfile::create(spec);
    if (!prof.ok()) { note_ = prof.error().detail; phase_ = Phase::Failed; return; }
    profile_ = prof.value();
    phase_ = Phase::Idle;
    if (!session_.beginPrepare(*profile_, sessionId_).ok()) { fail("Recording could not be prepared"); return; }
    const auto rep = session_.completePrepare(in);
    if (!rep.ok) { fail(firstIssue(rep)); return; }

    fs_ = std::make_unique<rec::StdioRecorderFs>(dir_);
    if (!journal_.open(dirF.getChildFile(rec::journalFileName(wallStart_, sessionId_, show_)).getFullPathName().toStdString()).ok()) {
        fail("The recording journal could not be created in the chosen folder");
        return;
    }
    rec_ = std::make_unique<rec::AudioOnlyRecorder>(*profile_, sessionId_, wallStart_, *fs_, journal_);
    auto st = rec_->start();
    if (!st.ok()) { fail(st.error().detail.empty() ? "The recording file could not be created" : st.error().detail); return; }
    syncSegments();
    nextDiskCheck_ = clock_.nowMonotonic() + ss::secToNs(5);
    freeMb_ = fb >> 20;
    phase_ = Phase::Active;
    if (cfg.recordVideo) startVideo(cfg, probe, frame, dir_);
    else videoNote_ = "Video recording is off in Setup";
}

void RecordRunner::startVideo(const MediaConfig&, const FfmpegProbe* probe, const ss::PixelBuffer* frame, const std::string& dir) {
    if (!probe || !probe->found) { videoNote_ = "Video off: " + (probe && !probe->reason.empty() ? probe->reason : std::string("ffmpeg was not found")) + ". Audio is still recorded"; return; }
    if (!procs_.auxSupported()) { videoNote_ = "Video off: not available on this system in this build. Audio is still recorded"; return; }
    if (!frame || frame->empty()) { videoNote_ = "Video off: no picture is being produced yet. Audio is still recorded"; return; }
    vw_ = std::clamp<std::uint32_t>(evenDown(frame->width), 160, 4096); vh_ = std::clamp<std::uint32_t>(evenDown(frame->height), 120, 4096);
    rec::RecordProfileSpec vs;
    vs.kind = rec::ProfileKind::VideoMatroska;
    vs.showName = show_;
    vs.audio = {rate_, ch_};
    vs.video.width = vw_; vs.video.height = vh_; vs.video.fpsNum = fps_; vs.video.fpsDen = 1;
    vs.outputDir = dir;
    auto vp = rec::RecordProfile::create(vs);
    if (!vp.ok()) { videoNote_ = "Video off: " + vp.error().detail; return; }
    rec::PrepareInputs in;
    in.path = {true, true, true};
    in.disk = {true, freeBytes()};
    in.sources.audioConfigured = in.sources.audioDeviceOk = true;
    in.sources.audioRateHz = rate_; in.sources.audioChannels = ch_;
    in.sources.videoConfigured = in.sources.videoSourceOk = true;
    in.sources.videoWidth = vw_; in.sources.videoHeight = vh_; in.sources.videoRateKnown = true; in.sources.videoFps = fps_;
    in.encoders = toProbeResults(*probe);
    const auto vr = rec::prepareChecks(vp.value(), in);
    if (!vr.ok || !vr.encoder) { videoNote_ = "Video off: " + firstIssue(vr) + ". Audio is still recorded"; return; }
    rec::RecordCommandInput ci(vp.value());
    ci.ffmpegPath = probe->path;
    ci.encoder = *vr.encoder;
    ci.video = {"pipe:0", vw_, vh_, fps_, 1, rec::PixelFormat::Rgba};
    ci.audio = {"pipe:3", rate_, ch_};
    ci.sessionId = sessionId_;
    ci.sessionStartWallUnixNs = wallStart_;
    auto cmd = rec::FfmpegCommandBuilder::buildRecord(ci);
    if (!cmd.ok()) { videoNote_ = "Video off: " + cmd.error().detail; return; }
    std::shared_ptr<ss::broadcast::IProcess> p(procs_.create().release());
    if (!p->start(cmd.value().argv).ok()) { videoNote_ = "Video off: the encoder could not be started. Audio is still recorded"; return; }
    proc_ = p;
    videoNote_ = vr.encoder->fallbackFromH264 ? vr.encoder->note : std::string();
}

void RecordRunner::syncSegments() {
    if (!rec_) return;
    const auto& names = rec_->fileNames();
    while (openedSegs_ < names.size()) {
        session_.onSegmentOpened(openedSegs_ + 1, names[openedSegs_], openedSegs_ == 0 ? 0 : rec_->mediaNs());
        ++openedSegs_;
    }
    const auto& closed = rec_->closedSegments();
    while (closedSegs_ < closed.size()) {
        const auto& b = closed[closedSegs_];
        session_.onSegmentClosed(b.index, b.frames * ch_ * 3 + rec::kWavHeaderBytes, b.endNs);
        ++closedSegs_;
    }
    session_.onBytesWritten(rec_->totalBytes());
}

void RecordRunner::onAudio(const AudioChunk& c) {
    if (phase_ != Phase::Active || !rec_) return;
    if (c.rateHz != rate_ || c.channels != ch_) { ++audioDropped_; session_.noteLoss(session_.elapsedNs(), 0, false, "Audio format changed"); return; }
    auto st = rec_->write(c.view());
    if (!st.ok()) {
        const bool full = st.error().category == ss::ErrorCategory::DiskFull;
        fail(full ? "The disk is full. Earlier parts of the recording are kept" : "The recording could not be written to disk");
        return;
    }
    syncSegments();
    if (proc_ && procs_.writeAux(*proc_, reinterpret_cast<const std::uint8_t*>(c.s.data()), c.s.size() * sizeof(float)) == false) {
        ++audioDropped_;
        videoNote_ = "The video encoder is not keeping up: audio was dropped from the video file (the WAV is complete)";
    }
}

void RecordRunner::onVideo(const ss::PixelBuffer& frame) {
    if (phase_ != Phase::Active || !proc_) return;
    fitRgba(frame, vw_, vh_, vbuf_);
    if (!procs_.writeStdin(*proc_, vbuf_.data(), vbuf_.size())) ++videoDropped_;
    else ++videoFrames_;
}

void RecordRunner::tick() {
    if (phase_ != Phase::Active && phase_ != Phase::Stopping) return;
    const ss::Ns now = clock_.nowMonotonic();
    if (proc_) {
        procs_.pumpWrites(*proc_);
        stderrTail_ += proc_->readStderr();
        proc_->readProgress();
        if (stderrTail_.size() > 400) stderrTail_.erase(0, stderrTail_.size() - 400);
        if (!proc_->running()) {
            const int code = proc_->exitCode();
            if (phase_ == Phase::Active) {
                videoNote_ = "The video encoder stopped (exit code " + std::to_string(code) + "). Audio is still being recorded";
                session_.noteLoss(session_.elapsedNs(), 0, false, "Video encoder exited");
            }
            proc_.reset();
        }
    }
    if (phase_ == Phase::Active && now >= nextDiskCheck_) {
        nextDiskCheck_ = now + ss::secToNs(5);
        const auto ds = session_.checkDisk(freeBytes());
        freeMb_ = ds.freeBytes >> 20;
        if (ds.level == ss::record::DiskLevel::Warning) note_ = ds.message;
        if (session_.state() == rec::State::Stopping) { requestStop(rec::StopReason::DiskLow); note_ = "Stopped: the disk is nearly full. The recording so far is saved"; }
    }
    if (phase_ == Phase::Stopping) {
        if (proc_ && now >= stopDeadline_) proc_->kill();
        if (!proc_) finalize();
    }
}

void RecordRunner::requestStop(rec::StopReason why) {
    if (phase_ != Phase::Active) return;
    stopReason_ = why;
    if (rec_) { rec_->stop(true); syncSegments(); }
    session_.requestStop(why);
    phase_ = Phase::Stopping;
    stopDeadline_ = clock_.nowMonotonic() + ss::secToNs(5);
    if (proc_) procs_.pumpWrites(*proc_), proc_->requestStop();
    else finalize();
}

void RecordRunner::finalize() {
    session_.onWriterStopped(true);
    juce::File dirF(juce::String::fromUTF8(dir_.c_str()));
    dirF.getChildFile(rec::metadataFileName(wallStart_, sessionId_, show_)).replaceWithText(juce::String::fromUTF8(session_.metadataJson({}).c_str()), false, false, "\n");
    if (!session_.summary().markers.empty())
        dirF.getChildFile(sessionId_ + "_markers.csv").replaceWithText(juce::String::fromUTF8(rec::markersCsv(session_.summary().markers).c_str()), false, false, "\n");
    journal_.close();
    phase_ = Phase::Done;
    if (note_.empty() || stopReason_ == rec::StopReason::User) {
        const auto n = session_.summary().segments.size();
        note_ = "Saved " + std::to_string(n) + (n == 1 ? " audio file" : " audio files") + (videoFrames_ ? " and video" : "");
    }
}

bool RecordRunner::addMarker() {
    if (phase_ != Phase::Active) return false;
    auto n = static_cast<std::uint32_t>(session_.summary().markers.size()) + 1;
    return session_.addMarker("Marker " + std::to_string(n)).ok();
}

RecordView RecordRunner::view() const {
    RecordView v;
    v.note = note_;
    if (!videoNote_.empty()) v.note += (v.note.empty() ? "" : ". ") + videoNote_;
    v.videoOn = proc_ != nullptr;
    v.freeDiskMb = freeMb_;
    const auto& sum = session_.summary();
    v.markers = static_cast<std::uint32_t>(sum.markers.size());
    v.segments = static_cast<std::uint32_t>(sum.segments.size());
    v.elapsedMs = static_cast<std::uint64_t>(std::max<ss::Ns>(0, session_.elapsedNs()) / ss::kNsPerMs);
    v.bytesWritten = rec_ ? rec_->totalBytes() : 0;
    switch (phase_) {
        case Phase::Idle: v.state = RecordUiState::Off; break;
        case Phase::Active: v.state = (rec_ && rec_->framesWritten() > 0) ? RecordUiState::Recording : RecordUiState::Preparing; break;
        case Phase::Stopping: v.state = RecordUiState::Stopping; break;
        case Phase::Done: v.state = RecordUiState::Off; break;
        case Phase::Failed: v.state = session_.state() == rec::State::Recoverable ? RecordUiState::Recoverable : RecordUiState::Failed; break;
    }
    return v;
}

}  // namespace ss::app
