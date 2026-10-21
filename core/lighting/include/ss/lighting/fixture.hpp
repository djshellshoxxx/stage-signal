// Fixture profiles and patches are DATA (model::FixtureProfile / model::FixturePatch), never scripts.
// This file: built-in profile library, validation, conflict detection and channel-domain helpers.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/lighting/addressing.hpp"
#include "ss/model/show_model.hpp"

namespace ss::lighting {

using model::ChannelFunction;
using model::FixtureChannel;
using model::FixturePatch;
using model::FixtureProfile;
using model::LightingProtocol;

// ---- Built-in library: generic dimmer, RGB, RGBW (+ a 5 channel RGB/dimmer/strobe fixture) ---------
constexpr const char* kProfileDimmer = "generic-dimmer";
constexpr const char* kProfileRgb = "generic-rgb";
constexpr const char* kProfileRgbw = "generic-rgbw";
constexpr const char* kProfileRgbStrobe = "generic-dimmer-rgb-strobe";
const std::vector<FixtureProfile>& builtinProfiles();
const FixtureProfile* findProfile(const std::vector<FixtureProfile>& profiles, const std::string& id);

std::size_t footprint(const FixtureProfile& p);                  // sum of byteWidth
bool isContinuous(ChannelFunction f);                            // may be faded: Intensity, colors, Pan, Tilt
bool isIntensityLike(ChannelFunction f);                         // Intensity, Red, Green, Blue, White
// Normalised 0..1 -> physical value in the channel's own domain (8 or 16 bit), honouring legalMin/legalMax.
std::uint32_t physicalFromNormalized(const FixtureChannel& c, float v);
std::uint32_t physicalMax(const FixtureChannel& c);              // 255 or 65535
// 8 bit configured value (default/safe/blackout) -> channel domain (coarse byte = value, fine byte = 0).
std::uint32_t physicalFromByteValue(const FixtureChannel& c, std::uint8_t v);

// ---- Validation -------------------------------------------------------------------------------------
enum class Severity : std::uint8_t { Warning, Error };
enum class IssueCode : std::uint8_t {
    ProfileInvalid, DuplicateProfileId, ProfileMissing, DuplicatePatchId, UniverseOutOfRange, StartOutOfRange,
    FootprintExceedsUniverse, Overlap, TooManyFixtures, TooManyUniverses
};
struct Issue {
    Severity severity = Severity::Warning;
    IssueCode code = IssueCode::ProfileInvalid;
    std::uint32_t patchId = 0, otherPatchId = 0;    // 0 = not applicable
    bool blocksArm = true;                          // every issue currently blocks arming; warnings still load
    std::string message;                            // sanitized, no user paths
};
struct PatchOptions {
    // Operator-resolved ownership: these patches yield (output nothing, take part in no conflict check).
    std::vector<std::uint32_t> disabledPatchIds;
};
struct PatchReport {
    std::vector<Issue> issues;
    bool canArm() const { for (const auto& i : issues) if (i.blocksArm) return false; return true; }
    bool hasErrors() const { for (const auto& i : issues) if (i.severity == Severity::Error) return true; return false; }
    std::size_t count(IssueCode c) const { std::size_t n = 0; for (const auto& i : issues) if (i.code == c) ++n; return n; }
};

std::vector<Issue> validateProfile(const FixtureProfile& p);
// Overlap between patches that are NOT the same shared-ownership mirror (same non-zero group, profile, universe
// and start) is a conflict: warning severity, blocksArm = true.
PatchReport validatePatches(const std::vector<FixtureProfile>& profiles, const std::vector<FixturePatch>& patches,
                            LightingProtocol protocol, const PatchOptions& opt = {},
                            std::vector<std::uint32_t>* acceptedPatchIds = nullptr);   // ids that can output (valid, enabled)

}  // namespace ss::lighting
