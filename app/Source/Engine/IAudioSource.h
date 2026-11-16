// Contract: audio capture adapter (app-adapters 3, APP-AD-01..03). Owner: engine-adapters-desktop.
// An IAudioSource owns a device (or the host connection) and delivers interleaved float blocks to an IAudioSink.
// The sink is the AudioIngress; pushBlock() is real-time safe and is called from the audio thread.
// In standalone the production source wraps AudioDeviceManager + AudioProcessorPlayer, so the sink call
// happens inside StageSignalProcessor::processBlock (the single entry, app-shell 4.2); in CLAP the host calls
// processBlock. Either way the sink contract is the same.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Engine/AdapterCommon.h"
#include "ss/base/types.hpp"

namespace ss::app {

struct AudioDeviceDesc {
    std::string typeName, deviceName;       // identity stored in the show: type + name + input channel count
    int inputChannels = 0;
    std::vector<double> sampleRates;
};

struct AudioStreamFormat {
    std::uint32_t rateHz = 0, channels = 0, bufferFrames = 0;
    double inputLatencyMs = 0.0;            // reported, not measured
};

class IAudioSink {
public:
    virtual ~IAudioSink() = default;
    // Audio thread. No allocation, lock, file, socket, UI or log call.
    virtual void pushBlock(const float* interleaved, std::uint32_t frames, std::uint32_t channels,
                           std::uint32_t rateHz, ss::Ns callbackEntryNs) = 0;
    // Non-audio thread: the stream was (re)opened. Bumps the generation so stale blocks are discarded.
    virtual void streamOpened(const AudioStreamFormat& fmt) = 0;
    virtual void streamLost() = 0;
};

class IAudioSource : public IAdapter {
public:
    virtual void setSink(IAudioSink* sink) = 0;                       // before start()
    virtual std::vector<AudioDeviceDesc> listDevices() = 0;           // UI thread, may block briefly
    virtual ss::Status selectDevice(const AudioDeviceDesc& d) = 0;    // never falls back to another device silently
    virtual AudioStreamFormat format() const = 0;                     // valid when health().state == Running
    virtual bool hasDevice() const = 0;
};

}  // namespace ss::app
