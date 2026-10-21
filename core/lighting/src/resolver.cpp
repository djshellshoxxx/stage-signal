#include "ss/lighting/resolver.hpp"

#include <algorithm>
#include <cmath>

namespace ss::lighting {

namespace {
float clamp01(float v) { return v > 0.f ? (v < 1.f ? v : 1.f) : 0.f; }   // NaN -> 0

float featureValue(const FeatureSnapshot& s, AudioSource src) {
    switch (src) {
        case AudioSource::BandLow: return clamp01(s.band[0]);
        case AudioSource::BandMid: return clamp01(s.band[1]);
        case AudioSource::BandHigh: return clamp01(s.band[2]);
        case AudioSource::Level: {
            const float db = std::max(s.rmsDb[0], s.rmsDb[1]);
            return clamp01((db + 60.f) / 60.f);                   // -60 dBFS .. 0 dBFS -> 0..1
        }
        case AudioSource::Onset: return s.onsetFired ? 1.f : clamp01(s.onsetStrength);
        case AudioSource::EnergyArc: return clamp01(s.energyArc);
    }
    return 0.f;
}

bool blackoutClass(ChannelFunction f) { return isIntensityLike(f) || f == ChannelFunction::Strobe; }

std::uint32_t blackoutPhysical(const FixtureChannel& c) {
    // Intensity/colour/strobe go to the profile blackout value; everything else (pan/tilt/macro/other) holds
    // its configured safe value, so a blackout never swings a head or selects a macro.
    return physicalFromByteValue(c, blackoutClass(c.function) ? c.blackoutValue : c.safeValue);
}

void writeSlot(UniverseFrame& f, std::size_t slot, std::uint8_t width, std::uint32_t phys) {
    if (width == 2) {
        if (slot + 1 < kDmxSlots) { f.slots[slot] = static_cast<std::uint8_t>(phys >> 8); f.slots[slot + 1] = static_cast<std::uint8_t>(phys & 0xFF); }
    } else if (slot < kDmxSlots) {
        f.slots[slot] = static_cast<std::uint8_t>(phys & 0xFF);
    }
}
}  // namespace

Status FixtureResolver::configure(const std::vector<FixtureProfile>& profiles, const std::vector<FixturePatch>& patches,
                                  LightingProtocol protocol, const PatchOptions& opt, const std::vector<AudioMapping>& mappings) {
    configured_ = false;
    chans_.clear(); maps_.clear(); universes_.clear(); report_ = PatchReport{};
    haveLast_ = false;
    if (patches.size() > kMaxFixtures) return makeError(ErrorCategory::ResourceExhausted, "more than 256 fixtures");
    if (profiles.size() > kMaxProfiles) return makeError(ErrorCategory::ResourceExhausted, "too many fixture profiles");
    if (mappings.size() > kMaxAudioMappings) return makeError(ErrorCategory::ResourceExhausted, "too many audio mappings");

    std::vector<std::uint32_t> accepted;
    report_ = validatePatches(profiles, patches, protocol, opt, &accepted);
    for (const auto& p : patches) {
        if (std::find(accepted.begin(), accepted.end(), p.id) == accepted.end()) continue;
        const FixtureProfile* prof = findProfile(profiles, p.profileId);
        if (!prof || !validateProfile(*prof).empty()) continue;   // invalid profile: excluded, already reported
        std::size_t uidx = 0;
        while (uidx < universes_.size() && universes_[uidx] != p.universe) ++uidx;
        if (uidx == universes_.size()) universes_.push_back(p.universe);
        std::size_t slot = static_cast<std::size_t>(p.startAddress) - 1;
        for (std::size_t i = 0; i < prof->channels.size(); ++i) {
            CChan c; c.fixtureId = p.id; c.universeIdx = uidx; c.slot = slot; c.ch = prof->channels[i]; c.channelIndex = i;
            chans_.push_back(c);
            slot += prof->channels[i].byteWidth;
        }
    }
    for (const auto& m : mappings) maps_.push_back(CMap{m, 0.f});
    configured_ = true;
    return okStatus();
}

void FixtureResolver::reset() {
    haveLast_ = false;
    for (auto& c : chans_) { c.primed = false; c.cur = 0; c.last = Layer::Default; }
    for (auto& m : maps_) m.env = 0.f;
}

void FixtureResolver::safeFrames(std::vector<UniverseFrame>& out) const {
    out.assign(universes_.size(), UniverseFrame{});
    for (std::size_t i = 0; i < universes_.size(); ++i) out[i].universe = universes_[i];
    for (const auto& c : chans_) writeSlot(out[c.universeIdx], c.slot, c.ch.byteWidth, blackoutPhysical(c.ch));
}

Layer FixtureResolver::sourceOf(std::uint32_t fixtureId, std::size_t channelIndex) const {
    for (const auto& c : chans_) if (c.fixtureId == fixtureId && c.channelIndex == channelIndex) return c.last;
    return Layer::Default;
}

void FixtureResolver::resolve(Ns now, const ResolveInputs& in, std::vector<UniverseFrame>& out, ResolveReport* rep) {
    const Ns dt = (haveLast_ && now > lastNow_) ? now - lastNow_ : 0;
    const bool first = !haveLast_;
    lastNow_ = now; haveLast_ = true;

    // ---- audio envelopes (lowest precedence layer) ----
    std::vector<char> mapActive(maps_.size(), 0);
    for (std::size_t i = 0; i < maps_.size(); ++i) {
        auto& m = maps_[i];
        if (!in.audio) { m.env = 0.f; continue; }
        mapActive[i] = 1;
        const float target = clamp01(m.m.gain * featureValue(*in.audio, m.m.source) + m.m.offset);
        if (first) { m.env = target; continue; }
        const double tauNs = static_cast<double>(msToNs(1)) * static_cast<double>(target > m.env ? m.m.attackMs : m.m.releaseMs);
        const double alpha = (tauNs + static_cast<double>(dt)) > 0.0 ? static_cast<double>(dt) / (tauNs + static_cast<double>(dt)) : 1.0;
        m.env += static_cast<float>(alpha) * (target - m.env);
    }

    out.assign(universes_.size(), UniverseFrame{});
    for (std::size_t i = 0; i < universes_.size(); ++i) out[i].universe = universes_[i];

    const BlackoutKind bk = in.emergencyBlackout ? BlackoutKind::Emergency
                            : in.lightingBlackout ? BlackoutKind::Lighting : BlackoutKind::None;
    bool strobeForced = false;

    for (auto& c : chans_) {
        const ChannelFunction fn = c.ch.function;
        // ---- layers: manual > cue > audio > default ----
        double desired = physicalFromByteValue(c.ch, c.ch.defaultValue);
        Layer layer = Layer::Default;
        bool found = false; float norm = 0.f;
        for (const auto& l : in.manual) if (l.fixtureId == c.fixtureId && l.has(fn)) { norm = l.v[static_cast<std::size_t>(fn)]; found = true; }
        if (found) layer = Layer::Manual;
        if (!found) {
            for (const auto& l : in.cue) if (l.fixtureId == c.fixtureId && l.has(fn)) { norm = l.v[static_cast<std::size_t>(fn)]; found = true; }
            if (found) layer = Layer::Cue;
        }
        if (!found) {
            float best = -1.f;
            for (std::size_t i = 0; i < maps_.size(); ++i) {
                const auto& m = maps_[i];
                if (!mapActive[i] || m.m.target != fn) continue;
                if (m.m.fixtureId != 0 && m.m.fixtureId != c.fixtureId) continue;
                best = std::max(best, m.env);
            }
            if (best >= 0.f) { norm = best; found = true; layer = Layer::Audio; }
        }
        if (found) desired = physicalFromNormalized(c.ch, norm);

        // ---- fade in the physical-channel domain, by function ----
        if (!c.primed || in.fadeNs <= 0 || !isContinuous(fn)) {
            c.cur = desired; c.primed = true;                 // discrete functions snap: no interpolation through macro ranges
        } else {
            const double step = static_cast<double>(physicalMax(c.ch)) * static_cast<double>(dt) / static_cast<double>(in.fadeNs);
            if (desired > c.cur) c.cur = std::min(desired, c.cur + step);
            else c.cur = std::max(desired, c.cur - step);
        }
        c.last = layer;

        // ---- strobe lock (REQ-S12), then blackout (never faded) ----
        std::uint32_t phys = static_cast<std::uint32_t>(std::lround(c.cur));
        if (in.strobingDisabled && fn == ChannelFunction::Strobe) {
            const std::uint32_t safe = physicalFromByteValue(c.ch, c.ch.safeValue);
            if (safe != phys) strobeForced = true;
            phys = safe;
        }
        if (bk != BlackoutKind::None) {
            phys = blackoutPhysical(c.ch);
            c.last = bk == BlackoutKind::Emergency ? Layer::EmergencyBlackout : Layer::LightingBlackout;
        }
        writeSlot(out[c.universeIdx], c.slot, c.ch.byteWidth, phys);
    }
    if (rep) { rep->blackout = bk; rep->strobeLockApplied = strobeForced; }
}

}  // namespace ss::lighting
