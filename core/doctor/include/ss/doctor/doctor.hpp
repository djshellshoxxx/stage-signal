// StageSignal doctor: "Show Doctor" preflight (DEC-13). A pure function from a plain-data snapshot of the
// environment to an ordered list of findings. It reads nothing, opens nothing and transmits nothing: no
// device, file, network or clock access exists in this module, and every one-click fix is a *safe* action
// from the central registry (it never starts recording, broadcast or lighting and never lifts a blackout).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "ss/base/actions.hpp"
#include "ss/perf/admission.hpp"
#include "ss/router/router.hpp"

namespace ss::doctor {

enum class Severity : std::uint8_t { Blocker = 0, Warning = 1, Info = 2 };
const char* severityName(Severity s);

struct Fix {
    ActionId action = ActionId::None;
    double value = 0.0;
    std::string label;          // button text, e.g. "Disarm lighting"
};

struct Finding {
    Severity severity = Severity::Info;
    std::string id;             // stable, dotted, e.g. "audio.no-device"; may end in ".<output id>"
    std::string message;        // plain English, what is wrong
    std::string why;            // one line: why it matters on stage
    std::optional<Fix> fix;     // optional one-click fix
};

// ---- snapshot (all plain data; the app fills it from the other modules) ----------------------
struct AudioFacts {
    bool deviceSelected = false, deviceOpen = false, deviceLost = false;
    bool permissionDenied = false, formatUnsupported = false;
    double silentSeconds = 0;               // continuous silence right now
    bool clipping = false;                  // input at/near full scale (IF-03 inputNearFullScale)
    std::uint32_t clipRunLength = 0;
};
struct DisplayFacts {
    std::vector<router::RouteInfo> routes;  // DisplayRouter::routes()
    unsigned displaysConnected = 0;
    bool controlsOnFallback = false;
};
struct DiskFacts {
    bool known = false;                     // false => free space could not be read
    std::uint64_t freeBytes = 0;
    bool recordingPlanned = false;
    std::uint64_t plannedBytes = 0;         // estimated size of the planned recording
};
struct LightingFacts {
    bool configured = false, armed = false, dryRun = false;
    std::uint32_t patchConflicts = 0;       // overlapping fixture address ranges
    std::uint32_t unpatchedFixtures = 0;    // cues reference fixtures that have no patch
    bool destinationSet = false;
};
struct MidiFacts { std::uint32_t mappingsTotal = 0, mappingsUnavailable = 0; };
struct MediaFacts { std::uint32_t missing = 0, invalid = 0; };
struct SafetyFacts {
    bool strobingDisabled = false;
    bool strobeContentPresent = false;      // flash-risk media, strobe effects or strobe-capable patched fixtures
    bool visualBlackoutLatched = false, lightingBlackoutLatched = false;
};
struct FeatureAdmission {
    std::string id;
    perf::AdmissionState state = perf::AdmissionState::Admitted;
    bool usedByShow = false;                // the show currently relies on this feature
    std::string reason;                     // first admission reason, sanitized
};
struct BenchmarkFacts {
    bool run = false;
    std::int64_t ageMs = 0;
    bool matchesConfig = true;              // profile hash equals the current configuration
    perf::Verdict verdict = perf::Verdict::Incomplete;
    std::vector<FeatureAdmission> features;
};
struct BroadcastFacts { bool configured = false; bool secretReferencePresent = false; };
struct ValidationFacts {
    std::uint32_t errors = 0, warnings = 0;
    std::vector<std::string> firstMessages; // sanitized, clipped by the doctor
};

struct EnvironmentSnapshot {
    AudioFacts audio;
    DisplayFacts displays;
    DiskFacts disk;
    LightingFacts lighting;
    MidiFacts midi;
    MediaFacts media;
    SafetyFacts safety;
    BenchmarkFacts benchmark;
    BroadcastFacts broadcast;
    ValidationFacts validation;
    bool networkDisabled = false;           // offline mode: nothing may leave this computer
};

struct DoctorOptions {
    double silenceWarnSeconds = 10.0;
    std::int64_t benchmarkMaxAgeMs = 30LL * 24 * 3600 * 1000;
    std::uint64_t diskMarginBytes = 1024ull * 1024 * 1024;   // free space that must remain after the recording
};

class Doctor {
public:
    // Deterministic: findings are ordered by severity (Blocker, Warning, Info) then by stable id.
    static std::vector<Finding> run(const EnvironmentSnapshot& snapshot);
    static std::vector<Finding> run(const EnvironmentSnapshot& snapshot, const DoctorOptions& options);
    static bool hasBlockers(const std::vector<Finding>& f);
    // The set of actions a finding may offer as its one-click fix.
    static bool isSafeFixAction(ActionId a);
};

}  // namespace ss::doctor
