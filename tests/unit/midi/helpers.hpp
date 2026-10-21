#pragma once
#include <vector>
#include "doctest/doctest.h"
#include "ss/midi/engine.hpp"

namespace testutil {
using namespace ss;
using namespace ss::midi;

inline model::MidiMapping mkMap(model::MidiMsgType type, std::uint8_t num, ActionId a,
                                model::EncoderMode enc = model::EncoderMode::Absolute, std::uint8_t ch = 0,
                                const char* identity = "usb:A", std::uint8_t vmin = 0, std::uint8_t vmax = 127) {
    model::MidiMapping m;
    m.deviceIdentity = identity; m.deviceName = "Controller A";
    m.channel = ch; m.type = type; m.number = num; m.valueMin = vmin; m.valueMax = vmax;
    m.encoder = enc; m.actionId = id(a);
    return m;
}

// One engine with input port 1 ("usb:A") open.
struct Rig {
    MidiEngine e;
    Generation g = 0;
    Rig() { g = e.openInputPort(1, "usb:A", "Controller A", 0); }
    std::uint32_t add(model::MidiMsgType t, std::uint8_t n, ActionId a,
                      model::EncoderMode enc = model::EncoderMode::Absolute, std::uint8_t ch = 0,
                      MappingOptions opt = {}) {
        auto r = e.mappings().add(mkMap(t, n, a, enc, ch), opt);
        REQUIRE(r.ok());
        return r.value();
    }
    void noteOn(std::uint8_t n, std::uint8_t v, Ns t, std::uint8_t ch = 0) { e.onMessage(1, g, makeNoteOn(ch, n, v), t); }
    void noteOff(std::uint8_t n, Ns t, std::uint8_t ch = 0) { e.onMessage(1, g, makeNoteOff(ch, n), t); }
    void cc(std::uint8_t n, std::uint8_t v, Ns t, std::uint8_t ch = 0) { e.onMessage(1, g, makeCC(ch, n, v), t); }
    void pc(std::uint8_t n, Ns t, std::uint8_t ch = 0) { e.onMessage(1, g, makeProgramChange(ch, n), t); }
    std::vector<Action> poll(Ns t) { std::vector<Action> v; e.poll(t, v); return v; }
};
}  // namespace testutil
