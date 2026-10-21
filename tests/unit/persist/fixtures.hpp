// Shared test fixtures for persist tests.
#pragma once
#include <string>
#include "nlohmann/json.hpp"
#include "ss/base/actions.hpp"
#include "ss/base/sha256.hpp"
#include "ss/persist/show_json.hpp"

namespace fx {

inline ss::model::ShowDocument sampleDoc(const std::string& name = "Friday set") {
    using namespace ss::model;
    ShowDocument d;
    d.uuid = "123e4567-e89b-12d3-a456-426614174000";
    d.name = name;
    d.appVersion = "0.1.0";
    d.mode = ss::ShowMode::Hybrid;
    d.seed = 18446744073709551615ull;   // needs the string encoding
    d.minDwellSec = 10; d.repeatHistory = 4; d.strobingDisabled = true;

    MediaRef m;
    m.id = 7; m.relativePath = "media/loop.mp4"; m.sha256 = ss::Sha256::hashHex("loop-bytes"); m.format = "mp4";
    m.width = 1280; m.height = 720; m.byteSize = 10; m.durationSec = 12.5; m.flashRisk = true;
    d.media.push_back(m);

    for (std::uint32_t i = 1; i <= 2; ++i) {
        SceneDef s;
        s.id = i; s.name = "Scene " + std::to_string(i); s.kind = i == 1 ? SceneKind::NeonGrid : SceneKind::Video;
        s.params.brightness = 0.75f; s.params.effectAmount[0] = 0.25f; s.params.prismSegments = 8;
        s.seed = 0xFFFFFFFFFFFFFFFEull; s.strobe = i == 2; s.costEstimate = 2.5f; s.tier = i == 1 ? EnergyTier::Drop : EnergyTier::Low;
        s.mediaId = i == 2 ? 7 : 0;
        d.scenes.push_back(s);
    }
    TextMessage t;
    t.id = 3; t.utf8 = "Welcome \xE2\x98\x83 to the show\nLine two"; t.color = Rgb{10, 20, 30}; t.scroll = true;
    d.messages.push_back(t);
    for (std::uint32_t i = 1; i <= 2; ++i) {
        CueDef c;
        c.id = i; c.name = "Cue " + std::to_string(i); c.sceneId = i; c.duration = 16; c.timeBase = TimeBase::Beats;
        c.transition = TransitionKind::FadeThroughBlack; c.transitionDuration = 2.0; c.mediaId = i == 2 ? 7 : 0; c.textId = i == 1 ? 3 : 0;
        c.lightingRefs = {1, 2}; c.waitForTrigger = i == 2;
        d.cues.push_back(c);
    }
    OutputDef o2;
    o2.id = 2; o2.label = "Wall"; o2.kind = ss::RouteKind::Duplicate; o2.duplicateOf = 1; o2.width = 1920; o2.height = 1080; o2.matchHint = "27 inch wall display";
    d.outputs.push_back(o2);
    MidiMapping mm;
    mm.id = 1; mm.deviceName = "Launch Pad"; mm.deviceIdentity = "USB\\VID_1234&PID_5678\\SERIAL0001"; mm.channel = 255; mm.type = MidiMsgType::ControlChange;
    mm.number = 21; mm.valueMin = 0; mm.valueMax = 127; mm.encoder = EncoderMode::RelativeTwosComplement; mm.actionId = ss::id(ss::ActionId::NextScene);
    d.midi.push_back(mm);
    FixtureProfile fp;
    fp.id = "generic-rgb"; fp.name = "Generic RGB"; fp.version = 2;
    for (auto f : {ChannelFunction::Intensity, ChannelFunction::Red, ChannelFunction::Green, ChannelFunction::Blue}) {
        FixtureChannel c; c.function = f; c.legalMax = 255; c.safeValue = 0; fp.channels.push_back(c);
    }
    d.fixtureProfiles.push_back(fp);
    FixturePatch pt; pt.id = 1; pt.profileId = "generic-rgb"; pt.label = "Front wash"; pt.universe = 1; pt.startAddress = 1; pt.group = 2;
    d.patches.push_back(pt);
    d.lighting.protocol = ss::model::LightingProtocol::Sacn; d.lighting.destination = "192.168.1.50"; d.lighting.rateHz = 30; d.lighting.sacnPriority = 120;
    d.quality.profileHash = "ab12cd34"; d.quality.locked = true;
    d.journal.push_back({1000, 1700000000000LL, "scene", "to Scene 1"});
    d.journal.push_back({2000, 1700000001000LL, "marker", "Song A"});
    return d;
}

inline nlohmann::json toJ(const ss::model::ShowDocument& d) {
    auto s = ss::persist::showToJson(d);
    return nlohmann::json::parse(s.value());
}

}  // namespace fx
