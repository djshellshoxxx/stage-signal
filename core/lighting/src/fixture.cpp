#include "ss/lighting/fixture.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace ss::lighting {

namespace {
FixtureChannel ch(ChannelFunction f) {
    FixtureChannel c;
    c.function = f;                       // legal 0..255, default/safe/blackout 0
    return c;
}
FixtureProfile mk(const char* id, const char* name, std::initializer_list<ChannelFunction> fns) {
    FixtureProfile p;
    p.id = id; p.name = name; p.version = 1;
    for (auto f : fns) p.channels.push_back(ch(f));
    return p;
}
}  // namespace

const std::vector<FixtureProfile>& builtinProfiles() {
    static const std::vector<FixtureProfile> lib = {
        mk(kProfileDimmer, "Generic dimmer (1 ch)", {ChannelFunction::Intensity}),
        mk(kProfileRgb, "Generic RGB (3 ch)", {ChannelFunction::Red, ChannelFunction::Green, ChannelFunction::Blue}),
        mk(kProfileRgbw, "Generic RGBW (4 ch)",
           {ChannelFunction::Red, ChannelFunction::Green, ChannelFunction::Blue, ChannelFunction::White}),
        mk(kProfileRgbStrobe, "Generic dimmer + RGB + strobe (5 ch)",
           {ChannelFunction::Intensity, ChannelFunction::Red, ChannelFunction::Green, ChannelFunction::Blue,
            ChannelFunction::Strobe}),
    };
    return lib;
}

const FixtureProfile* findProfile(const std::vector<FixtureProfile>& profiles, const std::string& id) {
    for (const auto& p : profiles) if (p.id == id) return &p;
    return nullptr;
}

std::size_t footprint(const FixtureProfile& p) {
    std::size_t n = 0;
    for (const auto& c : p.channels) n += c.byteWidth;
    return n;
}

bool isContinuous(ChannelFunction f) {
    switch (f) {
        case ChannelFunction::Intensity: case ChannelFunction::Red: case ChannelFunction::Green:
        case ChannelFunction::Blue: case ChannelFunction::White: case ChannelFunction::Pan: case ChannelFunction::Tilt:
            return true;
        default: return false;            // Strobe, Macro, Other: discrete; never interpolated
    }
}
bool isIntensityLike(ChannelFunction f) {
    return f == ChannelFunction::Intensity || f == ChannelFunction::Red || f == ChannelFunction::Green ||
           f == ChannelFunction::Blue || f == ChannelFunction::White;
}

std::uint32_t physicalMax(const FixtureChannel& c) { return c.byteWidth == 2 ? 65535u : 255u; }

std::uint32_t physicalFromByteValue(const FixtureChannel& c, std::uint8_t v) {
    return c.byteWidth == 2 ? static_cast<std::uint32_t>(v) << 8 : v;
}

std::uint32_t physicalFromNormalized(const FixtureChannel& c, float v) {
    if (!(v > 0.f)) v = 0.f;              // also catches NaN
    if (v > 1.f) v = 1.f;
    const std::uint32_t lo = c.byteWidth == 2 ? (static_cast<std::uint32_t>(c.legalMin) << 8) : c.legalMin;
    const std::uint32_t hi = c.byteWidth == 2 ? ((static_cast<std::uint32_t>(c.legalMax) << 8) | 0xFFu) : c.legalMax;
    if (hi <= lo) return lo;
    return lo + static_cast<std::uint32_t>(std::lround(static_cast<double>(v) * static_cast<double>(hi - lo)));
}

std::vector<Issue> validateProfile(const FixtureProfile& p) {
    std::vector<Issue> out;
    auto err = [&](const std::string& m) {
        Issue i; i.severity = Severity::Error; i.code = IssueCode::ProfileInvalid; i.message = "profile '" + p.id + "': " + m;
        out.push_back(i);
    };
    if (p.id.empty()) err("empty id");
    if (p.channels.empty()) err("no channels");
    if (p.channels.size() > kMaxProfileChannels) err("too many channels");
    if (footprint(p) > kDmxSlots) err("footprint exceeds 512");
    for (std::size_t i = 0; i < p.channels.size(); ++i) {
        const auto& c = p.channels[i];
        const std::string at = "channel " + std::to_string(i + 1) + ": ";
        if (c.byteWidth != 1 && c.byteWidth != 2) err(at + "byteWidth must be 1 or 2");
        if (c.legalMin > c.legalMax) err(at + "legalMin > legalMax");
        auto inLegal = [&](std::uint8_t v) { return v >= c.legalMin && v <= c.legalMax; };
        if (!inLegal(c.defaultValue)) err(at + "default outside legal range");
        if (!inLegal(c.safeValue)) err(at + "safe value outside legal range");
        if (!inLegal(c.blackoutValue)) err(at + "blackout value outside legal range");
        if (static_cast<std::uint8_t>(c.function) > static_cast<std::uint8_t>(ChannelFunction::Other)) err(at + "unknown function");
    }
    return out;
}

PatchReport validatePatches(const std::vector<FixtureProfile>& profiles, const std::vector<FixturePatch>& patches,
                            LightingProtocol protocol, const PatchOptions& opt,
                            std::vector<std::uint32_t>* acceptedPatchIds) {
    PatchReport rep;
    if (acceptedPatchIds) acceptedPatchIds->clear();
    auto add = [&](Severity s, IssueCode c, std::uint32_t a, std::uint32_t b, std::string m) {
        Issue i; i.severity = s; i.code = c; i.patchId = a; i.otherPatchId = b; i.message = std::move(m);
        rep.issues.push_back(std::move(i));
    };
    if (profiles.size() > kMaxProfiles) add(Severity::Error, IssueCode::ProfileInvalid, 0, 0, "too many profiles");
    std::set<std::string> seenProfiles;
    for (const auto& p : profiles) {
        if (!seenProfiles.insert(p.id).second) add(Severity::Error, IssueCode::DuplicateProfileId, 0, 0, "duplicate profile id '" + p.id + "'");
        for (auto& i : validateProfile(p)) rep.issues.push_back(i);
    }
    if (patches.size() > kMaxFixtures) {
        add(Severity::Error, IssueCode::TooManyFixtures, 0, 0, "more than 256 fixtures");
        return rep;                                              // bounds before any per-patch work
    }
    auto disabled = [&](std::uint32_t id) {
        return std::find(opt.disabledPatchIds.begin(), opt.disabledPatchIds.end(), id) != opt.disabledPatchIds.end();
    };

    struct Span { const FixturePatch* p; std::size_t first, last; };    // 1-based inclusive slots
    std::vector<Span> spans;
    std::set<std::uint32_t> seenIds;
    std::vector<std::uint16_t> universeOrder;
    for (const auto& p : patches) {
        if (!seenIds.insert(p.id).second) { add(Severity::Error, IssueCode::DuplicatePatchId, p.id, 0, "duplicate patch id"); continue; }
        if (disabled(p.id)) continue;
        const FixtureProfile* prof = findProfile(profiles, p.profileId);
        if (!prof) { add(Severity::Error, IssueCode::ProfileMissing, p.id, 0, "patch references unknown profile '" + p.profileId + "'"); continue; }
        const bool uniOk = protocol == LightingProtocol::ArtNet ? p.universe <= kArtNetMaxPortAddress
                                                                : (p.universe >= kSacnMinUniverse && p.universe <= kSacnMaxUniverse);
        if (!uniOk) {
            add(Severity::Error, IssueCode::UniverseOutOfRange, p.id, 0,
                protocol == LightingProtocol::ArtNet ? "Art-Net Port-Address must be 0..32767 (zero-based wire value)"
                                                     : "sACN universe must be 1..63999 (one-based)");
            continue;
        }
        if (p.startAddress < 1 || p.startAddress > kDmxSlots) { add(Severity::Error, IssueCode::StartOutOfRange, p.id, 0, "start address must be 1..512"); continue; }
        const std::size_t fp = footprint(*prof);
        if (fp == 0 || p.startAddress + fp - 1 > kDmxSlots) {
            add(Severity::Error, IssueCode::FootprintExceedsUniverse, p.id, 0, "start + footprint exceeds 512");
            continue;
        }
        if (std::find(universeOrder.begin(), universeOrder.end(), p.universe) == universeOrder.end()) universeOrder.push_back(p.universe);
        const auto uniIdx = static_cast<std::size_t>(std::find(universeOrder.begin(), universeOrder.end(), p.universe) - universeOrder.begin());
        if (uniIdx >= kMaxUniverses) {
            add(Severity::Error, IssueCode::TooManyUniverses, p.id, 0, "more than 4 universes are not admitted");
            continue;
        }
        spans.push_back({&p, p.startAddress, p.startAddress + fp - 1});
        if (acceptedPatchIds) acceptedPatchIds->push_back(p.id);
    }
    for (std::size_t i = 0; i < spans.size(); ++i) {
        for (std::size_t j = i + 1; j < spans.size(); ++j) {
            const auto& a = spans[i]; const auto& b = spans[j];
            if (a.p->universe != b.p->universe) continue;
            if (a.last < b.first || b.last < a.first) continue;
            const bool mirror = a.p->group != 0 && a.p->group == b.p->group && a.p->profileId == b.p->profileId &&
                                a.p->startAddress == b.p->startAddress;
            if (mirror) continue;                                // shared ownership declared via group
            add(Severity::Warning, IssueCode::Overlap, a.p->id, b.p->id,
                "patches " + std::to_string(a.p->id) + " and " + std::to_string(b.p->id) + " overlap in universe " +
                    std::to_string(a.p->universe) + "; resolve ownership before arming");
        }
    }
    return rep;
}

}  // namespace ss::lighting
