// Contract: MIDI port enumeration and I/O (app-adapters 5, APP-AD-06). Owner: engine-adapters-desktop.
// Input callbacks run on an OS thread: stamp IClock::now() and forward raw bytes only (no parsing, no core calls).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Engine/AdapterCommon.h"
#include "ss/base/time.hpp"

namespace ss::app {

struct MidiPortInfo {
    std::string identity;     // stable (USB ids/serial/endpoint id) where the OS offers one
    std::string name;         // fallback only
    bool isInput = true;
};

class IMidiInputSink {
public:
    virtual ~IMidiInputSink() = default;
    // OS MIDI thread. Must not allocate, lock or call into core.
    virtual void onMidiBytes(std::uint32_t portId, const std::uint8_t* bytes, std::uint32_t n, ss::Ns stampNs) = 0;
};

class IMidiOutPort {
public:
    virtual ~IMidiOutPort() = default;
    virtual ss::Status send(const std::uint8_t* bytes, std::uint32_t n) = 0;   // MIDI-OUT thread only
};

class IMidiPortProvider : public IAdapter {
public:
    virtual std::vector<MidiPortInfo> listPorts() = 0;                          // polled <= 1 s for hot-plug
    virtual ss::Result<std::uint32_t> openInput(const std::string& identity, IMidiInputSink* sink) = 0;  // returns portId
    virtual void closeInput(std::uint32_t portId) = 0;
    virtual ss::Result<IMidiOutPort*> openOutput(const std::string& identity) = 0;  // provider keeps ownership
    virtual void closeOutput(IMidiOutPort* p) = 0;
};

}  // namespace ss::app
