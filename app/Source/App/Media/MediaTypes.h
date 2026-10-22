// Shared small types for the MEDIA worker (record + broadcast runners). Owner: engine-media-record.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/types.hpp"
#include "Engine/IProcessSupervisor.h"
#include "ss/broadcast/settings.hpp"
#include "ss/record/profile.hpp"

namespace ss::app {

// Operator-chosen settings. Set through MediaWorker::configure(); copied into each session when it starts.
struct MediaConfig {
    std::string ffmpegPath;                 // empty = look next to the app, then on PATH
    std::string outputDir;                  // recordings folder; empty = <Documents>/StageSignal Recordings
    std::string showName = "show";
    bool recordVideo = true;                // video file next to the WAV when ffmpeg is usable
    std::uint32_t recordFps = 30;
    ss::OutputId programOutput = 1;         // which output is recorded and broadcast
    ss::broadcast::BroadcastSettings broadcast;
};

struct AudioChunk {                         // owning copy of one processed block (pooled by the worker)
    std::vector<float> s;
    std::uint32_t frames = 0, channels = 0, rateHz = 0, flags = 0;
    std::uint64_t firstSampleIndex = 0;
    ss::Ns captureNs = 0;
    ss::Generation generation = 0;
    ss::AudioBlockView view() const { return {s.data(), frames, channels, rateHz, firstSampleIndex, captureNs, flags, generation}; }
};

// Letterboxed nearest-neighbour fit of an RGBA frame into dw x dh (opaque black bars). `out` is resized.
void fitRgba(const ss::PixelBuffer& src, std::uint32_t dw, std::uint32_t dh, std::vector<std::uint8_t>& out);
// Re-map interleaved float audio to `outCh` channels (mono duplicates, extra channels dropped).
void remapChannels(const AudioChunk& in, std::uint32_t outCh, std::vector<float>& out);

// What the record core needs to choose an encoder: listed + verified flags from the probe.
ss::record::ProbeResults toProbeResults(const FfmpegProbe& p);

}  // namespace ss::app
