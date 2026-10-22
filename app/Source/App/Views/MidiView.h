// UI-facing MIDI status. Struct owner: engine-adapters-desktop. Producer: MidiScheduler::fillView().
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ss::app {

struct MidiPortRow { std::string identity, name; bool isInput = true; bool open = false; };

struct MidiMappingRow {
    std::uint32_t id = 0;
    std::string device;                // stored device name
    int channel = 0;                   // 0..15, 255 = any
    int type = 0;                      // ss::model::MidiMsgType value
    int number = 0, valueMin = 0, valueMax = 127, encoder = 0;
    std::uint32_t actionId = 0;
    std::string actionName;
    bool deviceAvailable = false;
};

struct MidiLearnView {
    bool active = false, captured = false;
    std::string text;                  // "Waiting", or "Channel 1, CC 7" once captured
};

struct MidiView {
    bool outputEnabled = false;      // MIDI-OUT is disarmed by default
    bool clockOn = false;
    std::uint32_t inputPorts = 0, outputPorts = 0;
    std::uint64_t ticks = 0, messagesSent = 0, inputDropped = 0;
    std::vector<MidiPortRow> ports;    // enumerated by the provider (IO thread, every ~2 s)
    std::vector<MidiMappingRow> mappings;
    MidiLearnView learn;
    std::string note;
};

}  // namespace ss::app
