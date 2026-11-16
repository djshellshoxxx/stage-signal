// Contract: child process supervision for ffmpeg (app-adapters 8). Owner: engine-media-record.
// Implements the broadcast core's IProcessFactory so BroadcastSession can use it directly; adds discovery and
// pipe feeding that the record path needs. argv vector only, never a shell string; environment whitelist.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "Engine/AdapterCommon.h"
#include "ss/broadcast/process.hpp"

namespace ss::app {

struct FfmpegProbe {
    bool found = false;
    std::string versionLine;               // sanitised
    bool hasH264 = false, hasAac = false, hasOpus = false;
    // Additive (engine-media-record): what `-encoders` listed and which encoders passed a one-frame test encode.
    std::string path;                      // resolved executable; never shown to the operator
    std::string encodersText;              // raw `-encoders` output (parse with ss::record::parseFfmpegEncoders)
    bool h264Verified = false, mjpegVerified = false;   // any eligible H.264 / MJPEG encoder passed a test encode
    std::vector<std::string> verifiedEncoders;
    std::string reason;                    // operator-facing cause when found == false
};

class IProcessSupervisor : public IAdapter, public ss::broadcast::IProcessFactory {
public:
    // Discovery order: user path, next to executable, PATH. Runs `-version`/`-encoders` with a 3 s timeout.
    virtual FfmpegProbe probeFfmpeg(const std::string& userPath) = 0;
    // Bounded, non-blocking write into the child's stdin; false when the queue is full (frame dropped and counted).
    virtual bool writeStdin(ss::broadcast::IProcess& p, const std::uint8_t* data, std::size_t n) = 0;
    virtual std::size_t liveChildren() const = 0;
    // Additive: second input pipe (the child sees it as fd 3 / `pipe:3`); POSIX only. Same bounded semantics as stdin.
    virtual bool writeAux(ss::broadcast::IProcess&, const std::uint8_t*, std::size_t) { return false; }
    virtual bool auxSupported() const { return false; }
    // Additive: push queued bytes into the pipes now (called by the owner of the process every few ms).
    virtual void pumpWrites(ss::broadcast::IProcess&) {}
};

}  // namespace ss::app
