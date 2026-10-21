// T-DMX: fixture profiles, validation, patch conflicts, resolver precedence, safe values, fades, audio, strobe lock.
#include "doctest/doctest.h"
#include "test_util.hpp"

using namespace ss;
using namespace ss::lighting;
using namespace ss::lighting::testutil;

TEST_CASE("REQ-L01: built-in profile library validates and has the documented footprints") {
    const auto& lib = builtinProfiles();
    REQUIRE(lib.size() >= 3);
    CHECK(footprint(*findProfile(lib, kProfileDimmer)) == 1);
    CHECK(footprint(*findProfile(lib, kProfileRgb)) == 3);
    CHECK(footprint(*findProfile(lib, kProfileRgbw)) == 4);
    CHECK(footprint(*findProfile(lib, kProfileRgbStrobe)) == 5);
    for (const auto& p : lib) CHECK(validateProfile(p).empty());
    CHECK(findProfile(lib, "nope") == nullptr);
}

TEST_CASE("REQ-L01: profile validation rejects bad data") {
    FixtureProfile p = *findProfile(builtinProfiles(), kProfileRgb);
    CHECK(validateProfile(p).empty());
    FixtureProfile q = p; q.id.clear();                               CHECK_FALSE(validateProfile(q).empty());
    q = p; q.channels.clear();                                        CHECK_FALSE(validateProfile(q).empty());
    q = p; q.channels[0].byteWidth = 3;                               CHECK_FALSE(validateProfile(q).empty());
    q = p; q.channels[0].byteWidth = 0;                               CHECK_FALSE(validateProfile(q).empty());
    q = p; q.channels[0].legalMin = 200; q.channels[0].legalMax = 100; CHECK_FALSE(validateProfile(q).empty());
    q = p; q.channels[0].legalMin = 10; q.channels[0].legalMax = 20;  // default 0 outside legal range
    CHECK_FALSE(validateProfile(q).empty());
    q = p; q.channels.assign(kMaxProfileChannels + 1, p.channels[0]); CHECK_FALSE(validateProfile(q).empty());
    q = p; q.channels.assign(257, p.channels[0]); for (auto& c : q.channels) c.byteWidth = 2;   // 514 slots
    CHECK_FALSE(validateProfile(q).empty());
}

TEST_CASE("REQ-L01: patch bounds: start + footprint <= 512") {
    const auto& lib = builtinProfiles();
    auto run = [&](std::uint16_t start, const char* prof, LightingProtocol proto = LightingProtocol::ArtNet) {
        return validatePatches(lib, {patch(1, prof, 0, start)}, proto);
    };
    CHECK(run(1, kProfileRgb).canArm());
    CHECK(run(510, kProfileRgb).canArm());                        // 510..512
    CHECK_FALSE(run(511, kProfileRgb).canArm());                  // 511..513
    CHECK(run(511, kProfileRgb).count(IssueCode::FootprintExceedsUniverse) == 1);
    CHECK(run(512, kProfileDimmer).canArm());
    CHECK_FALSE(run(512, kProfileRgb).canArm());
    CHECK(run(0, kProfileDimmer).count(IssueCode::StartOutOfRange) == 1);
    CHECK(run(513, kProfileDimmer).count(IssueCode::StartOutOfRange) == 1);
    CHECK(run(500, kProfileRgbw).canArm());
    CHECK_FALSE(run(510, kProfileRgbw).canArm());
}

TEST_CASE("REQ-L01: universe range is checked per protocol, zero/one-based explicit") {
    const auto& lib = builtinProfiles();
    auto run = [&](std::uint16_t uni, LightingProtocol proto) { return validatePatches(lib, {patch(1, kProfileDimmer, uni, 1)}, proto); };
    CHECK(run(0, LightingProtocol::ArtNet).canArm());             // wire 0 is valid for Art-Net
    CHECK(run(32767, LightingProtocol::ArtNet).canArm());
    CHECK_FALSE(run(32768, LightingProtocol::ArtNet).canArm());
    CHECK_FALSE(run(0, LightingProtocol::Sacn).canArm());         // there is no sACN universe 0
    CHECK(run(1, LightingProtocol::Sacn).canArm());
    CHECK(run(63999, LightingProtocol::Sacn).canArm());
    CHECK_FALSE(run(64000, LightingProtocol::Sacn).canArm());
    CHECK(run(0, LightingProtocol::Sacn).count(IssueCode::UniverseOutOfRange) == 1);
}

TEST_CASE("REQ-L01: overlapping patches warn and cannot arm; disabled/mirror resolves ownership") {
    const auto& lib = builtinProfiles();
    // RGB at 1..3 and dimmer at 3: overlap
    auto rep = validatePatches(lib, {patch(1, kProfileRgb, 0, 1), patch(2, kProfileDimmer, 0, 3)}, LightingProtocol::ArtNet);
    CHECK(rep.count(IssueCode::Overlap) == 1);
    CHECK(rep.issues[0].severity == Severity::Warning);
    CHECK_FALSE(rep.hasErrors());
    CHECK_FALSE(rep.canArm());
    // adjacent, no overlap
    CHECK(validatePatches(lib, {patch(1, kProfileRgb, 0, 1), patch(2, kProfileDimmer, 0, 4)}, LightingProtocol::ArtNet).canArm());
    // same channels in different universes: fine
    CHECK(validatePatches(lib, {patch(1, kProfileRgb, 0, 1), patch(2, kProfileRgb, 1, 1)}, LightingProtocol::ArtNet).canArm());
    // operator resolves ownership by disabling one
    PatchOptions opt; opt.disabledPatchIds = {2};
    CHECK(validatePatches(lib, {patch(1, kProfileRgb, 0, 1), patch(2, kProfileDimmer, 0, 3)}, LightingProtocol::ArtNet, opt).canArm());
    // declared mirror: same non-zero group, profile and start
    CHECK(validatePatches(lib, {patch(1, kProfileRgb, 0, 1, 7), patch(2, kProfileRgb, 0, 1, 7)}, LightingProtocol::ArtNet).canArm());
    // same group but offset start is NOT a mirror
    CHECK_FALSE(validatePatches(lib, {patch(1, kProfileRgb, 0, 1, 7), patch(2, kProfileRgb, 0, 2, 7)}, LightingProtocol::ArtNet).canArm());
    // identical placement with group 0 is a conflict
    CHECK_FALSE(validatePatches(lib, {patch(1, kProfileRgb, 0, 1), patch(2, kProfileRgb, 0, 1)}, LightingProtocol::ArtNet).canArm());
}

TEST_CASE("REQ-L01: other patch problems are reported") {
    const auto& lib = builtinProfiles();
    auto rep = validatePatches(lib, {patch(1, "missing", 0, 1)}, LightingProtocol::ArtNet);
    CHECK(rep.count(IssueCode::ProfileMissing) == 1);
    rep = validatePatches(lib, {patch(1, kProfileDimmer, 0, 1), patch(1, kProfileDimmer, 0, 2)}, LightingProtocol::ArtNet);
    CHECK(rep.count(IssueCode::DuplicatePatchId) == 1);
    std::vector<FixturePatch> many;
    for (std::uint32_t i = 1; i <= 257; ++i) many.push_back(patch(i, kProfileDimmer, 0, 1));
    CHECK(validatePatches(lib, many, LightingProtocol::ArtNet).count(IssueCode::TooManyFixtures) == 1);
    rep = validatePatches(lib, {patch(1, kProfileDimmer, 0, 1), patch(2, kProfileDimmer, 1, 1), patch(3, kProfileDimmer, 2, 1),
                                patch(4, kProfileDimmer, 3, 1), patch(5, kProfileDimmer, 4, 1)}, LightingProtocol::ArtNet);
    CHECK(rep.count(IssueCode::TooManyUniverses) == 1);
    auto libDup = lib; libDup.push_back(lib[0]);
    CHECK(validatePatches(libDup, {}, LightingProtocol::ArtNet).count(IssueCode::DuplicateProfileId) == 1);
}

TEST_CASE("REQ-L01: profiles map channels to intensity/colour at the patched addresses") {
    FixtureResolver r;
    REQUIRE(r.configure(builtinProfiles(), {patch(1, kProfileRgbw, 0, 10), patch(2, kProfileDimmer, 1, 512)},
                        LightingProtocol::ArtNet, {}, {}).ok());
    REQUIRE(r.universes().size() == 2);
    ResolveInputs in;
    in.cue = {look(1).set(ChannelFunction::Red, 1.f).set(ChannelFunction::Green, 0.5f).set(ChannelFunction::Blue, 0.f).set(ChannelFunction::White, 1.f),
              look(2).set(ChannelFunction::Intensity, 1.f)};
    std::vector<UniverseFrame> out;
    r.resolve(0, in, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].slots[9] == 255);       // address 10 = red
    CHECK(out[0].slots[10] == 128);      // green 0.5 -> round(127.5) = 128
    CHECK(out[0].slots[11] == 0);
    CHECK(out[0].slots[12] == 255);      // address 13 = white
    CHECK(out[0].slots[8] == 0);         // unpatched
    CHECK(out[1].universe == 1);
    CHECK(out[1].slots[511] == 255);
}

TEST_CASE("REQ-L04: resolve precedence manual > cue > audio > default") {
    FixtureResolver r;
    AudioMapping m; m.source = AudioSource::BandLow; m.target = ChannelFunction::Intensity; m.attackMs = 0; m.releaseMs = 0;
    REQUIRE(r.configure(builtinProfiles(), {patch(1, kProfileDimmer, 0, 1)}, LightingProtocol::ArtNet, {}, {m}).ok());
    std::vector<UniverseFrame> out;
    ResolveInputs in;
    in.audio = silentFeatures(); in.audio->band[0] = 0.25f;
    r.resolve(0, in, out);                                     // audio only
    CHECK(out[0].slots[0] == 64);
    CHECK(r.sourceOf(1, 0) == Layer::Audio);
    in.cue = {look(1).set(ChannelFunction::Intensity, 0.5f)};
    r.resolve(kNsPerMs, in, out);                              // cue beats audio
    CHECK(out[0].slots[0] == 128);
    CHECK(r.sourceOf(1, 0) == Layer::Cue);
    in.manual = {look(1).set(ChannelFunction::Intensity, 1.f)};
    r.resolve(2 * kNsPerMs, in, out);                          // manual beats cue
    CHECK(out[0].slots[0] == 255);
    CHECK(r.sourceOf(1, 0) == Layer::Manual);
    in.lightingBlackout = true;
    r.resolve(3 * kNsPerMs, in, out);                          // lighting blackout beats manual
    CHECK(out[0].slots[0] == 0);
    CHECK(r.sourceOf(1, 0) == Layer::LightingBlackout);
    in.emergencyBlackout = true;
    ResolveReport rep;
    r.resolve(4 * kNsPerMs, in, out, &rep);                    // emergency beats lighting blackout
    CHECK(rep.blackout == BlackoutKind::Emergency);
    CHECK(r.sourceOf(1, 0) == Layer::EmergencyBlackout);
    in.emergencyBlackout = false; in.lightingBlackout = false; in.manual.clear(); in.cue.clear(); in.audio.reset();
    r.resolve(5 * kNsPerMs, in, out);                          // nothing set: profile default
    CHECK(out[0].slots[0] == 0);
    CHECK(r.sourceOf(1, 0) == Layer::Default);
}

TEST_CASE("REQ-L04: fixture-aware blackout uses safe values, not all-zero") {
    FixtureResolver r;
    REQUIRE(r.configure({movingHead()}, {patch(1, "head", 0, 1)}, LightingProtocol::ArtNet, {}, {}).ok());
    // channel order: Pan(16) coarse/fine, Tilt, Intensity, Strobe, Macro
    ResolveInputs in;
    in.cue = {look(1).set(ChannelFunction::Pan, 1.f).set(ChannelFunction::Tilt, 1.f).set(ChannelFunction::Intensity, 1.f)
                  .set(ChannelFunction::Strobe, 1.f).set(ChannelFunction::Macro, 1.f)};
    std::vector<UniverseFrame> out;
    r.resolve(0, in, out);
    CHECK(out[0].slots[0] == 255); CHECK(out[0].slots[1] == 255);       // pan 16 bit full
    CHECK(out[0].slots[3] == 255);                                      // intensity full
    CHECK(out[0].slots[4] == 255);                                      // strobe asked for
    CHECK(out[0].slots[5] == 9);                                        // macro max legal
    in.lightingBlackout = true;
    r.resolve(kNsPerMs, in, out);
    CHECK(out[0].slots[0] == 128); CHECK(out[0].slots[1] == 0);         // pan safe value, 16 bit = 128 << 8
    CHECK(out[0].slots[2] == 100);                                      // tilt safe value held
    CHECK(out[0].slots[3] == 0);                                        // intensity blackout
    CHECK(out[0].slots[4] == 0);                                        // strobe blackout (shutter closed / no strobe)
    CHECK(out[0].slots[5] == 7);                                        // macro safe value, not zero (zero is illegal here)
    std::vector<UniverseFrame> safe;
    r.safeFrames(safe);
    CHECK(safe[0].slots == out[0].slots);                               // graceful stop frames == blackout frames
}

TEST_CASE("REQ-S12: strobe lock forces strobe channels to the safe value from every layer") {
    FixtureResolver r;
    AudioMapping m; m.source = AudioSource::Onset; m.target = ChannelFunction::Strobe; m.attackMs = 0; m.releaseMs = 0;
    REQUIRE(r.configure(builtinProfiles(), {patch(1, kProfileRgbStrobe, 0, 1), patch(2, kProfileRgbStrobe, 0, 10)},
                        LightingProtocol::ArtNet, {}, {m}).ok());
    ResolveInputs in;
    in.cue = {look(1).set(ChannelFunction::Strobe, 1.f).set(ChannelFunction::Red, 1.f)};
    in.manual = {look(2).set(ChannelFunction::Strobe, 0.8f)};
    in.audio = silentFeatures(); in.audio->onsetFired = true;
    std::vector<UniverseFrame> out;
    ResolveReport rep;
    r.resolve(0, in, out, &rep);
    CHECK(out[0].slots[4] == 255);                       // strobe allowed: cue
    CHECK(out[0].slots[13] == 204);                      // manual
    CHECK_FALSE(rep.strobeLockApplied);
    in.strobingDisabled = true;
    r.resolve(kNsPerMs, in, out, &rep);
    CHECK(out[0].slots[4] == 0);                         // locked to safe value
    CHECK(out[0].slots[13] == 0);
    CHECK(out[0].slots[1] == 255);                       // non-strobe channels untouched
    CHECK(rep.strobeLockApplied);
    // audio-only strobe is locked too
    in.cue.clear(); in.manual.clear();
    r.resolve(2 * kNsPerMs, in, out, &rep);
    CHECK(out[0].slots[4] == 0);
    in.strobingDisabled = false;
    r.resolve(3 * kNsPerMs, in, out, &rep);
    CHECK(out[0].slots[4] == 255);                       // lock released
    // the lock uses the profile safe value, which need not be zero
    auto prof = *findProfile(builtinProfiles(), kProfileRgbStrobe);
    prof.id = "strobe-safe-10"; prof.channels[4].safeValue = 10;
    FixtureResolver r2;
    REQUIRE(r2.configure({prof}, {patch(1, "strobe-safe-10", 0, 1)}, LightingProtocol::ArtNet, {}, {}).ok());
    ResolveInputs in2; in2.strobingDisabled = true; in2.cue = {look(1).set(ChannelFunction::Strobe, 1.f)};
    r2.resolve(0, in2, out);
    CHECK(out[0].slots[4] == 10);
}

TEST_CASE("REQ-L01: fades run in the physical domain per function; discrete channels snap") {
    FixtureResolver r;
    REQUIRE(r.configure({movingHead()}, {patch(1, "head", 0, 1)}, LightingProtocol::ArtNet, {}, {}).ok());
    std::vector<UniverseFrame> out;
    ResolveInputs in; in.fadeNs = kNsPerSec;
    in.cue = {look(1).set(ChannelFunction::Intensity, 0.f).set(ChannelFunction::Pan, 0.f).set(ChannelFunction::Macro, 0.f)};
    r.resolve(0, in, out);                                         // primes at the targets
    in.cue = {look(1).set(ChannelFunction::Intensity, 1.f).set(ChannelFunction::Pan, 1.f).set(ChannelFunction::Macro, 1.f)};
    r.resolve(100 * kNsPerMs, in, out);                            // 10 % of the fade
    CHECK(out[0].slots[3] == 26);                                  // 255 * 0.1 = 25.5 -> 26 (linear in DMX units)
    const unsigned pan16 = (out[0].slots[0] << 8) | out[0].slots[1];
    CHECK(pan16 == 6554);                                          // 65535 * 0.1 = 6553.5 (16 bit domain)
    CHECK(out[0].slots[5] == 9);                                   // macro snapped to target; never interpolated 5,6,7,8
    r.resolve(600 * kNsPerMs, in, out);
    CHECK(out[0].slots[3] == 153);                                 // 60 %
    r.resolve(2 * kNsPerSec, in, out);
    CHECK(out[0].slots[3] == 255);
    // a fade down is symmetric and never overshoots
    in.cue = {look(1).set(ChannelFunction::Intensity, 0.f)};
    r.resolve(2 * kNsPerSec + 500 * kNsPerMs, in, out);
    CHECK(out[0].slots[3] == 128);                                 // 255 - 127.5 = 127.5 -> 128
    // blackout is never faded
    in.lightingBlackout = true;
    r.resolve(2 * kNsPerSec + 501 * kNsPerMs, in, out);
    CHECK(out[0].slots[3] == 0);
    CHECK(out[0].slots[0] == 128);
}

TEST_CASE("REQ-L01: legal ranges bound the physical mapping") {
    FixtureResolver r;
    REQUIRE(r.configure({movingHead()}, {patch(1, "head", 0, 1)}, LightingProtocol::ArtNet, {}, {}).ok());
    std::vector<UniverseFrame> out;
    ResolveInputs in;
    in.cue = {look(1).set(ChannelFunction::Macro, 0.f)};
    r.resolve(0, in, out);
    CHECK(out[0].slots[5] == 5);                                   // legalMin
    in.cue = {look(1).set(ChannelFunction::Macro, 5.f)};           // out-of-range request clamps
    r.resolve(kNsPerMs, in, out);
    CHECK(out[0].slots[5] == 9);
    in.cue = {look(1).set(ChannelFunction::Intensity, -3.f)};
    r.resolve(2 * kNsPerMs, in, out);
    CHECK(out[0].slots[3] == 0);
}

TEST_CASE("REQ-L02: audio mapping consumes FeatureSnapshot with deterministic smoothing") {
    FixtureResolver r;
    AudioMapping m; m.source = AudioSource::BandMid; m.target = ChannelFunction::Intensity;
    m.gain = 2.f; m.offset = 0.f; m.attackMs = 100; m.releaseMs = 100;
    REQUIRE(r.configure(builtinProfiles(), {patch(1, kProfileDimmer, 0, 1)}, LightingProtocol::ArtNet, {}, {m}).ok());
    std::vector<UniverseFrame> out;
    ResolveInputs in; in.audio = silentFeatures(); in.audio->band[1] = 0.5f;   // target = 1.0
    r.resolve(0, in, out);
    CHECK(out[0].slots[0] == 255);                                 // first sample primes the envelope
    in.audio->band[1] = 0.f;                                       // target 0, release 100 ms
    r.resolve(100 * kNsPerMs, in, out);                            // alpha = 100/(100+100) = 0.5
    CHECK(out[0].slots[0] == 128);
    r.resolve(200 * kNsPerMs, in, out);                            // 0.25
    CHECK(out[0].slots[0] == 64);
    in.audio.reset();                                              // no snapshot: layer inactive -> default
    r.resolve(300 * kNsPerMs, in, out);
    CHECK(out[0].slots[0] == 0);
    CHECK(r.sourceOf(1, 0) == Layer::Default);
    // identical inputs give identical bytes
    FixtureResolver r2;
    REQUIRE(r2.configure(builtinProfiles(), {patch(1, kProfileDimmer, 0, 1)}, LightingProtocol::ArtNet, {}, {m}).ok());
    std::vector<UniverseFrame> out2;
    ResolveInputs in2; in2.audio = silentFeatures(); in2.audio->band[1] = 0.5f;
    r2.resolve(0, in2, out2);
    CHECK(out2[0].slots[0] == 255);
}

TEST_CASE("REQ-L01: audio mapping targets one fixture or all; sources map to 0..1") {
    FixtureResolver r;
    AudioMapping a; a.fixtureId = 2; a.source = AudioSource::Level; a.target = ChannelFunction::Intensity; a.attackMs = 0; a.releaseMs = 0;
    AudioMapping b; b.source = AudioSource::EnergyArc; b.target = ChannelFunction::Red; b.attackMs = 0; b.releaseMs = 0;
    REQUIRE(r.configure(builtinProfiles(), {patch(1, kProfileDimmer, 0, 1), patch(2, kProfileDimmer, 0, 2), patch(3, kProfileRgb, 0, 3)},
                        LightingProtocol::ArtNet, {}, {a, b}).ok());
    ResolveInputs in; in.audio = silentFeatures();
    in.audio->rmsDb[0] = -30.f; in.audio->rmsDb[1] = -120.f; in.audio->energyArc = 1.f;
    std::vector<UniverseFrame> out;
    r.resolve(0, in, out);
    CHECK(out[0].slots[0] == 0);          // fixture 1 not targeted
    CHECK(out[0].slots[1] == 128);        // fixture 2: -30 dBFS -> 0.5
    CHECK(out[0].slots[2] == 255);        // fixture 3 red from energy arc (all fixtures with a Red channel)
    CHECK(out[0].slots[3] == 0);
}

TEST_CASE("REQ-L01: resolver configuration limits are checked before allocation") {
    FixtureResolver r;
    std::vector<FixturePatch> many;
    for (std::uint32_t i = 1; i <= 257; ++i) many.push_back(patch(i, kProfileDimmer, 0, 1));
    CHECK_FALSE(r.configure(builtinProfiles(), many, LightingProtocol::ArtNet, {}, {}).ok());
    std::vector<AudioMapping> maps(kMaxAudioMappings + 1);
    CHECK_FALSE(r.configure(builtinProfiles(), {}, LightingProtocol::ArtNet, {}, maps).ok());
    // invalid patches are excluded from output but conflicts still resolve
    REQUIRE(r.configure(builtinProfiles(), {patch(1, kProfileRgb, 0, 511), patch(2, kProfileDimmer, 0, 1)}, LightingProtocol::ArtNet, {}, {}).ok());
    CHECK_FALSE(r.armable());
    CHECK(r.report().count(IssueCode::FootprintExceedsUniverse) == 1);
    CHECK(r.universes().size() == 1);
}
