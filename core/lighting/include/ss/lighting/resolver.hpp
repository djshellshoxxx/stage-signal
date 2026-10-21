// Fixture resolver. Precedence (spec section 14), highest first:
//   emergency blackout > lighting blackout > manual fixture override > programmed cue > audio modulation > profile default
// Blackout is fixture-aware (profile blackout/safe values, not "all zero"). The strobe lock (REQ-S12) forces
// every Strobe channel to its profile safe value regardless of which layer asked for strobing.
// Fades run in the physical channel domain (8/16 bit) per function; discrete functions (Strobe, Macro, Other)
// snap and are never interpolated through undefined ranges. Blackout and the strobe lock are never faded.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/base/types.hpp"
#include "ss/lighting/fixture.hpp"

namespace ss::lighting {

constexpr std::size_t kFunctionCount = static_cast<std::size_t>(ChannelFunction::Other) + 1;

// Normalised (0..1) values per channel function for one fixture. Used for cues and manual overrides.
struct FixtureLook {
    std::uint32_t fixtureId = 0;                 // FixturePatch::id
    std::uint16_t mask = 0;                      // bit per ChannelFunction that this look sets
    std::array<float, kFunctionCount> v{};
    FixtureLook& set(ChannelFunction f, float value) {
        mask = static_cast<std::uint16_t>(mask | (1u << static_cast<unsigned>(f)));
        v[static_cast<std::size_t>(f)] = value; return *this;
    }
    bool has(ChannelFunction f) const { return (mask >> static_cast<unsigned>(f)) & 1u; }
};

enum class AudioSource : std::uint8_t { BandLow, BandMid, BandHigh, Level, Onset, EnergyArc };
struct AudioMapping {
    std::uint32_t fixtureId = 0;                 // 0 = every fixture that has the target function
    AudioSource source = AudioSource::BandLow;
    ChannelFunction target = ChannelFunction::Intensity;
    float gain = 1.f, offset = 0.f;              // value = clamp01(gain * feature + offset)
    float attackMs = 20.f, releaseMs = 250.f;    // one-pole smoothing, deterministic (alpha = dt / (tau + dt))
};

enum class Layer : std::uint8_t { Default, Audio, Cue, Manual, LightingBlackout, EmergencyBlackout };
enum class BlackoutKind : std::uint8_t { None, Lighting, Emergency };

struct ResolveInputs {
    bool emergencyBlackout = false, lightingBlackout = false, strobingDisabled = false;
    std::vector<FixtureLook> manual, cue;
    std::optional<FeatureSnapshot> audio;        // sampled at lighting rate, never raw callback data
    Ns fadeNs = 0;                               // time for a full-scale traverse; 0 = snap
};

struct UniverseFrame {
    std::uint16_t universe = 0;                  // wire value in the configured protocol (Art-Net 0-based / sACN 1-based)
    std::array<std::uint8_t, kDmxSlots> slots{};
};

struct ResolveReport {
    BlackoutKind blackout = BlackoutKind::None;
    bool strobeLockApplied = false;              // at least one Strobe channel was forced to its safe value
};

class FixtureResolver {
public:
    // Validates (see validatePatches) and compiles the patch. Invalid patches are excluded from output and
    // reported; conflicting ones still resolve but armable() is false. Fails only on structural limits.
    Status configure(const std::vector<FixtureProfile>& profiles, const std::vector<FixturePatch>& patches,
                     LightingProtocol protocol, const PatchOptions& opt, const std::vector<AudioMapping>& mappings);
    const PatchReport& report() const { return report_; }
    bool armable() const { return configured_ && report_.canArm(); }
    const std::vector<std::uint16_t>& universes() const { return universes_; }

    // Computes the latest state of every patched universe. `now` must be monotonic (a regression counts as dt 0).
    void resolve(Ns now, const ResolveInputs& in, std::vector<UniverseFrame>& out, ResolveReport* rep = nullptr);
    // Fixture-aware safe frames (identical to a lighting blackout); does not advance fade or audio state.
    void safeFrames(std::vector<UniverseFrame>& out) const;

    // Which layer set the value of channel `channelIndex` (0-based, profile order) of a fixture in the last resolve.
    Layer sourceOf(std::uint32_t fixtureId, std::size_t channelIndex) const;
    void reset();                                // forget fades and audio envelopes (next resolve snaps)

private:
    struct CChan {
        std::uint32_t fixtureId; std::size_t universeIdx; std::size_t slot;   // slot 0-based
        FixtureChannel ch; std::size_t channelIndex;
        Layer last = Layer::Default; double cur = 0; bool primed = false;
    };
    struct CMap { AudioMapping m; float env = 0.f; };
    bool configured_ = false;
    PatchReport report_;
    std::vector<std::uint16_t> universes_;
    std::vector<CChan> chans_;
    std::vector<CMap> maps_;
    Ns lastNow_ = 0; bool haveLast_ = false;
};

}  // namespace ss::lighting
