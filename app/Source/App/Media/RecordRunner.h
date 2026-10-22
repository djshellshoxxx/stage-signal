// Recording session driver (record.md, app-adapters 9). Owner: engine-media-record. WORKER THREAD ONLY.
// WAV is always written by the core AudioOnlyRecorder. Video is an additional ffmpeg process (Matroska segments)
// when ffmpeg is usable; if not, the feature is off with a stated cause and the WAV continues.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include "App/Media/MediaTypes.h"
#include "App/Views/RecordView.h"
#include "Engine/IProcessSupervisor.h"
#include "ss/base/rng.hpp"
#include "ss/record/session.hpp"
#include "ss/record/wav.hpp"

namespace ss::app {

class RecordRunner {
public:
    RecordRunner(const ss::IClock& clock, IProcessSupervisor& procs) : clock_(clock), procs_(procs), session_(clock) {}

    // frame may be null (no picture yet): video is then off with a stated reason. probe may be null (not looked for).
    void begin(const MediaConfig& cfg, const FfmpegProbe* probe, std::uint32_t audioRate, std::uint32_t audioChannels,
               const ss::PixelBuffer* frame);
    void onAudio(const AudioChunk& c);
    void onVideo(const ss::PixelBuffer& frame);
    void tick();                                       // ffmpeg polling, disk check, stop progress
    void requestStop(ss::record::StopReason why);
    bool addMarker();
    bool active() const { return phase_ == Phase::Active || phase_ == Phase::Stopping; }
    bool stopping() const { return phase_ == Phase::Stopping; }
    bool wantsVideo() const { return phase_ == Phase::Active && proc_ != nullptr; }
    std::uint32_t fps() const { return fps_; }
    RecordView view() const;

private:
    enum class Phase { Idle, Active, Stopping, Done, Failed };
    void fail(const std::string& why);
    void syncSegments();
    void startVideo(const MediaConfig& cfg, const FfmpegProbe* probe, const ss::PixelBuffer* frame, const std::string& dir);
    void finalize();
    std::uint64_t freeBytes() const;

    const ss::IClock& clock_;
    IProcessSupervisor& procs_;
    ss::record::RecordSession session_;
    Phase phase_ = Phase::Idle;
    std::string note_, videoNote_, dir_, show_, sessionId_;
    std::optional<ss::record::RecordProfile> profile_;
    std::unique_ptr<ss::record::StdioRecorderFs> fs_;
    ss::record::FileJournalSink journal_;
    std::unique_ptr<ss::record::AudioOnlyRecorder> rec_;
    std::shared_ptr<ss::broadcast::IProcess> proc_;
    std::vector<std::uint8_t> vbuf_;
    std::uint32_t rate_ = 0, ch_ = 0, vw_ = 0, vh_ = 0, fps_ = 30, openedSegs_ = 0, closedSegs_ = 0;
    std::uint64_t videoDropped_ = 0, audioDropped_ = 0, videoFrames_ = 0, freeMb_ = 0;
    ss::Ns wallStart_ = 0, nextDiskCheck_ = 0, stopDeadline_ = 0, lastLossNs_ = 0;
    ss::record::StopReason stopReason_ = ss::record::StopReason::None;
    std::string stderrTail_;
    bool fileNote_ = false;
};

}  // namespace ss::app
