// Contract: child process supervision for ffmpeg (app-adapters 8). Owner: engine-media-record.
// Implements the broadcast core's IProcessFactory so BroadcastSession can use it directly; adds discovery and
// pipe feeding that the record path needs. argv vector only, never a shell string; environment whitelist.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "Engine/AdapterCommon.h"
#include "ss/broadcast/process.hpp"

namespace ss::app {

struct FfmpegProbe {
    bool found = false;
    std::string versionLine;               // sanitised
    bool hasH264 = false, hasAac = false, hasOpus = false;
};

class IProcessSupervisor : public IAdapter, public ss::broadcast::IProcessFactory {
public:
    // Discovery order: user path, next to executable, PATH. Runs `-version`/`-encoders` with a 3 s timeout.
    virtual FfmpegProbe probeFfmpeg(const std::string& userPath) = 0;
    // Bounded, non-blocking write into the child's stdin; false when the queue is full (frame dropped and counted).
    virtual bool writeStdin(ss::broadcast::IProcess& p, const std::uint8_t* data, std::size_t n) = 0;
    virtual std::size_t liveChildren() const = 0;
};

}  // namespace ss::app
