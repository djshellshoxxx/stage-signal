// Shared helpers for the lighting tests.
#pragma once
#include <string>
#include <vector>
#include "ss/base/types.hpp"
#include "ss/lighting/engine.hpp"
#include "ss/lighting/fixture.hpp"
#include "ss/lighting/resolver.hpp"

namespace ss::lighting::testutil {

inline FixturePatch patch(std::uint32_t id, const std::string& profile, std::uint16_t universe, std::uint16_t start,
                          std::uint32_t group = 0) {
    FixturePatch p; p.id = id; p.profileId = profile; p.universe = universe; p.startAddress = start; p.group = group;
    return p;
}
inline FixtureLook look(std::uint32_t fixtureId) { FixtureLook l; l.fixtureId = fixtureId; return l; }

inline FeatureSnapshot silentFeatures() {
    FeatureSnapshot s;
    s.band[0] = s.band[1] = s.band[2] = 0.f;
    s.onsetStrength = 0.f; s.onsetFired = false; s.energyArc = 0.f; s.silent = false;
    return s;
}

// Moving head: Pan(16 bit, safe 128), Tilt (safe 100), Intensity, Strobe, Macro (legal 5..9, default 5, safe 7).
// Slots: 1-2 pan coarse/fine, 3 tilt, 4 intensity, 5 strobe, 6 macro.
inline FixtureProfile movingHead() {
    FixtureProfile p; p.id = "head"; p.name = "Test moving head"; p.version = 1;
    FixtureChannel pan; pan.function = ChannelFunction::Pan; pan.byteWidth = 2; pan.safeValue = 128; pan.defaultValue = 128;
    FixtureChannel tilt; tilt.function = ChannelFunction::Tilt; tilt.safeValue = 100; tilt.defaultValue = 100;
    FixtureChannel inten; inten.function = ChannelFunction::Intensity;
    FixtureChannel strobe; strobe.function = ChannelFunction::Strobe;
    FixtureChannel macro; macro.function = ChannelFunction::Macro; macro.legalMin = 5; macro.legalMax = 9;
    macro.defaultValue = 5; macro.safeValue = 7; macro.blackoutValue = 7;
    p.channels = {pan, tilt, inten, strobe, macro};
    return p;
}

}  // namespace ss::lighting::testutil
