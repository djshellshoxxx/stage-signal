// Bounded, process-isolated GIF/H.264/MJPEG playback; driven only by the MEDIA clock.
#pragma once
#include <functional>
#include <memory>
#include "Engine/Media/NativeProcess.h"
#include "ss/media/validate.hpp"
#include "ss/base/types.hpp"

namespace ss::app {
class ClipDecoder final {
public:
    using Probe = std::function<CapturedRun(const std::vector<std::string>&, int)>;
    explicit ClipDecoder(IProcessSupervisor& processes, const IClock& clock, Probe probe = {});
    ~ClipDecoder() { stop(); }
    ss::Status open(const std::string& path, const FfmpegProbe& ffmpeg, double speed = 1., bool loop = false);
    void tick(Ns now);
    void stop();
    void setPaused(bool paused, Ns now);
    bool paused() const { return paused_; }
    double speed() const { return speed_; }
    bool looping() const { return looping_; }
    std::shared_ptr<const PixelBuffer> frame() const { return newest_; }  // MEDIA thread only
    const std::string& note() const { return note_; }
    bool failed() const { return failed_; }
    bool ended() const { return ended_; }
private:
    IProcessSupervisor& processes_;
    const IClock& clock_;
    Probe probe_;
    std::unique_ptr<ss::broadcast::IProcess> child_;
    std::shared_ptr<const PixelBuffer> newest_;
    PixelBuffer working_;
    std::string pending_, note_;
    std::size_t frameBytes_ = 0;
    std::uint64_t nextFrame_ = 0;
    Ns openedAt_ = 0, startedAt_ = 0, lastProgress_ = 0;
    Ns pausedAt_ = 0;
    double speed_ = 1.;
    bool looping_ = false, paused_ = false;
    bool haveStart_ = false, failed_ = false, ended_ = false;
};
}  // namespace ss::app
