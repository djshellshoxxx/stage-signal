// UI-facing MIDI status. Struct owner: engine-adapters-desktop. Producer: MidiScheduler::fillView().
#pragma once
#include <cstdint>
#include <string>

namespace ss::app {

struct MidiView {
    bool outputEnabled = false;      // MIDI-OUT is disarmed by default
    bool clockOn = false;
    std::uint32_t inputPorts = 0, outputPorts = 0;
    std::uint64_t ticks = 0, messagesSent = 0, inputDropped = 0;
    std::string note;
};

}  // namespace ss::app
