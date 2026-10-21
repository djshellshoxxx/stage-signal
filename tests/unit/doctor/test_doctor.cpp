#include "doctest/doctest.h"
#include <algorithm>
#include <type_traits>
#include "ss/base/rng.hpp"
#include "ss/doctor/doctor.hpp"

using namespace ss;
using namespace ss::doctor;
using ss::router::DisplayRouter;
using ss::router::PhysicalDisplay;
using ss::router::RouteStatus;

namespace {
PhysicalDisplay D(const char* identity, const char* connector, int w = 1920, int h = 1080, bool primary = false) {
    PhysicalDisplay d; d.identity = identity; d.connector = connector; d.width = w; d.height = h; d.primary = primary;
    return d;
}
model::OutputDef audience(OutputId id = 1, const char* label = "Main") {
    model::OutputDef o; o.id = id; o.label = label; o.kind = RouteKind::Audience;
    return o;
}

// A fully healthy rig: audio running, controls + audience display, benchmark fresh, nothing risky.
struct Rig {
    DisplayRouter router;
    EnvironmentSnapshot snap;
    Rig() {
        REQUIRE(router.setOutputs({audience()}).ok());
        REQUIRE(router.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("PROJ", "h1")}).ok());
        REQUIRE(router.assignControls("LAP", "eDP-1").ok());
        REQUIRE(router.assignOutput(1, "PROJ", "h1").ok());
        sync();
        snap.audio.deviceSelected = snap.audio.deviceOpen = true;
        snap.benchmark.run = true; snap.benchmark.matchesConfig = true; snap.benchmark.ageMs = 3LL * 24 * 3600 * 1000;
        snap.benchmark.verdict = perf::Verdict::Pass;
    }
    void sync() {
        snap.displays.routes = router.routes();
        snap.displays.displaysConnected = static_cast<unsigned>(router.topology().size());
        snap.displays.controlsOnFallback = router.controlsOnFallback();
    }
    std::vector<Finding> run() { return Doctor::run(snap); }
};

const Finding* find(const std::vector<Finding>& f, const std::string& id) {
    for (const Finding& x : f) if (x.id == id) return &x;
    return nullptr;
}
bool has(const std::vector<Finding>& f, const std::string& id) { return find(f, id) != nullptr; }
}  // namespace

TEST_CASE("Doctor: a healthy environment yields no findings") {
    Rig g;
    auto f = g.run();
    for (const auto& x : f) INFO(x.id);
    CHECK(f.empty());
    CHECK_FALSE(Doctor::hasBlockers(f));
}

TEST_CASE("Doctor: every finding is complete and plain English") {
    // Everything wrong at once.
    EnvironmentSnapshot s;
    s.audio.deviceSelected = true; s.audio.deviceOpen = true; s.audio.silentSeconds = 30; s.audio.clipping = true;
    s.disk.recordingPlanned = true; s.disk.known = true; s.disk.freeBytes = 100; s.disk.plannedBytes = 1000;
    s.lighting.configured = s.lighting.armed = true; s.lighting.patchConflicts = 2; s.lighting.unpatchedFixtures = 3;
    s.midi.mappingsTotal = 5; s.midi.mappingsUnavailable = 2;
    s.media.missing = 1; s.media.invalid = 1;
    s.safety.strobeContentPresent = true; s.safety.visualBlackoutLatched = true; s.safety.lightingBlackoutLatched = true;
    s.broadcast.configured = true; s.networkDisabled = true;
    s.validation.errors = 2; s.validation.firstMessages = {"scene 3 has no content"};
    auto f = Doctor::run(s);
    CHECK(f.size() >= 15);
    for (const Finding& x : f) {
        INFO(x.id);
        CHECK_FALSE(x.id.empty());
        CHECK_FALSE(x.message.empty());
        CHECK_FALSE(x.why.empty());
        CHECK(x.message.find("0x") == std::string::npos);
        CHECK(x.message.find("\\") == std::string::npos);
        CHECK(x.message.find("/") == std::string::npos);
        if (x.fix) { CHECK_FALSE(x.fix->label.empty()); CHECK(Doctor::isSafeFixAction(x.fix->action)); }
    }
}

TEST_CASE("Doctor: ordering is deterministic (severity, then id) and stable across runs") {
    EnvironmentSnapshot s;
    s.media.missing = 1; s.midi.mappingsUnavailable = 1; s.networkDisabled = true;
    s.broadcast.configured = true; s.validation.errors = 1; s.audio.permissionDenied = true;
    auto a = Doctor::run(s), b = Doctor::run(s);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) { CHECK(a[i].id == b[i].id); CHECK(a[i].message == b[i].message); }
    for (std::size_t i = 1; i < a.size(); ++i) {
        const bool ordered = a[i - 1].severity < a[i].severity || (a[i - 1].severity == a[i].severity && a[i - 1].id <= a[i].id);
        CHECK(ordered);
    }
    CHECK(a.front().severity == Severity::Blocker);
    CHECK(a.back().severity == Severity::Info);
}

TEST_CASE("Doctor audio: no device, permission, lost, format and not-open are blockers") {
    EnvironmentSnapshot s;
    auto f = Doctor::run(s);
    CHECK(find(f, "audio.no-device")->severity == Severity::Blocker);
    s.audio.deviceSelected = true;
    CHECK(find(Doctor::run(s), "audio.not-open")->severity == Severity::Blocker);
    s.audio.deviceOpen = true;
    CHECK_FALSE(has(Doctor::run(s), "audio.not-open"));
    s.audio.deviceLost = true;
    CHECK(find(Doctor::run(s), "audio.device-lost")->severity == Severity::Blocker);
    s.audio.deviceLost = false; s.audio.formatUnsupported = true;
    CHECK(find(Doctor::run(s), "audio.format-unsupported")->severity == Severity::Blocker);
    s.audio.formatUnsupported = false; s.audio.permissionDenied = true;
    f = Doctor::run(s);
    CHECK(find(f, "audio.permission-denied")->severity == Severity::Blocker);
    CHECK_FALSE(has(f, "audio.no-device"));                // only the root cause is reported
}

TEST_CASE("Doctor audio: silence and clipping are warnings with thresholds") {
    Rig g;
    g.snap.audio.silentSeconds = 9.9;
    CHECK_FALSE(has(g.run(), "audio.silent"));
    g.snap.audio.silentSeconds = 10.0;
    auto f = g.run();
    REQUIRE(has(f, "audio.silent"));
    CHECK(find(f, "audio.silent")->severity == Severity::Warning);
    DoctorOptions o; o.silenceWarnSeconds = 30;
    CHECK_FALSE(has(Doctor::run(g.snap, o), "audio.silent"));
    g.snap.audio.silentSeconds = 0; g.snap.audio.clipping = true;
    CHECK(find(g.run(), "audio.clipping")->severity == Severity::Warning);
    g.snap.audio.deviceLost = true;                        // no silence/clipping noise when the device itself is gone
    g.snap.audio.silentSeconds = 99;
    f = g.run();
    CHECK_FALSE(has(f, "audio.silent")); CHECK_FALSE(has(f, "audio.clipping"));
}

TEST_CASE("Doctor displays: disconnected audience output is a blocker with an identify-displays fix") {
    Rig g;
    REQUIRE(g.router.setTopology({D("LAP", "eDP-1", 1366, 768, true)}).ok());
    g.sync();
    auto f = g.run();
    const Finding* x = find(f, "display.disconnected.1");
    REQUIRE(x);
    CHECK(x->severity == Severity::Blocker);
    REQUIRE(x->fix.has_value());
    CHECK(x->fix->action == ActionId::IdentifyDisplays);
    CHECK(x->message.find("Main") != std::string::npos);
    CHECK(Doctor::hasBlockers(f));
    // when the display comes back the finding disappears
    REQUIRE(g.router.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("PROJ", "h1")}).ok());
    g.sync();
    CHECK_FALSE(has(g.run(), "display.disconnected.1"));
}

TEST_CASE("Doctor displays: ambiguous identical monitors need confirmation") {
    DisplayRouter r;
    REQUIRE(r.setOutputs({audience()}).ok());
    REQUIRE(r.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("TV", "h1"), D("TV", "h2")}).ok());
    REQUIRE(r.assignOutput(1, "TV", "h1").ok());
    router::RouterConfig saved = r.config();
    DisplayRouter r2;
    REQUIRE(r2.setOutputs({audience()}).ok());
    REQUIRE(r2.applyConfig(saved).ok());
    REQUIRE(r2.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("TV", "h1"), D("TV", "h2")}).ok());
    EnvironmentSnapshot s;
    s.audio.deviceSelected = s.audio.deviceOpen = true;
    s.benchmark.run = true; s.benchmark.verdict = perf::Verdict::Pass;
    s.displays.routes = r2.routes(); s.displays.displaysConnected = 3;
    auto f = Doctor::run(s);
    const Finding* x = find(f, "display.needs-confirmation.1");
    REQUIRE(x);
    CHECK(x->severity == Severity::Blocker);
    CHECK(x->fix->action == ActionId::IdentifyDisplays);
}

TEST_CASE("Doctor displays: single display, unassigned, suspended, disabled, fallback and diagnostics") {
    {   // single display: warning, not blocker, reported once
        DisplayRouter r;
        REQUIRE(r.setOutputs({audience(1), audience(2, "Second")}).ok());
        REQUIRE(r.setTopology({D("ONLY", "eDP-1", 1280, 800, true)}).ok());
        EnvironmentSnapshot s; s.audio.deviceSelected = s.audio.deviceOpen = true; s.benchmark.run = true;
        s.displays.routes = r.routes(); s.displays.displaysConnected = 1;
        auto f = Doctor::run(s);
        CHECK(find(f, "display.single-display")->severity == Severity::Warning);
        CHECK(std::count_if(f.begin(), f.end(), [](const Finding& x) { return x.id == "display.single-display"; }) == 1);
    }
    {   // two displays but nothing assigned: audience would see nothing
        DisplayRouter r;
        REQUIRE(r.setOutputs({audience()}).ok());
        REQUIRE(r.setTopology({D("A", "c1", 1366, 768, true), D("B", "c2")}).ok());
        EnvironmentSnapshot s; s.audio.deviceSelected = s.audio.deviceOpen = true; s.benchmark.run = true;
        s.displays.routes = r.routes(); s.displays.displaysConnected = 2;
        auto f = Doctor::run(s);
        CHECK(find(f, "display.unassigned.1")->severity == Severity::Blocker);
    }
    {   // controls lost their display: fallback warning and the paused audience blocker
        Rig g;
        REQUIRE(g.router.setTopology({D("PROJ", "h1", 1024, 768, true)}).ok());
        g.sync();
        auto f = g.run();
        CHECK(find(f, "display.controls-fallback")->severity == Severity::Warning);
        CHECK(find(f, "display.suspended.1")->severity == Severity::Blocker);
    }
    {   // capability-disabled extra output explains the limit
        DisplayRouter r;
        REQUIRE(r.setOutputs({audience(1), audience(2, "Second")}).ok());
        REQUIRE(r.setTopology({D("L", "eDP-1", 1366, 768, true), D("P1", "h1"), D("P2", "h2")}).ok());
        REQUIRE(r.assignOutput(1, "P1", "h1").ok());
        REQUIRE(r.assignOutput(2, "P2", "h2").ok());
        router::CapabilityLimits cap; cap.maxVisualOutputs = 1; cap.reason = "BEN-02 measured one output";
        r.setCapability(cap);
        EnvironmentSnapshot s; s.audio.deviceSelected = s.audio.deviceOpen = true; s.benchmark.run = true;
        s.displays.routes = r.routes(); s.displays.displaysConnected = 3;
        const auto df = Doctor::run(s);
        const Finding* x = find(df, "display.disabled.2");
        REQUIRE(x);
        CHECK(x->severity == Severity::Warning);
        CHECK(x->message.find("BEN-02 measured one output") != std::string::npos);
    }
    {   // diagnostics on an audience output
        Rig g;
        REQUIRE(g.router.setDiagnostics(1, true).ok());
        g.sync();
        CHECK(find(g.run(), "display.audience-diagnostics.1")->severity == Severity::Warning);
    }
    {   // mirror lost is only information
        Rig g;
        REQUIRE(g.router.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("PROJ", "h1"), D("MIR", "h2")}).ok());
        REQUIRE(g.router.assignMirror("MIR", "h2").ok());
        REQUIRE(g.router.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("PROJ", "h1")}).ok());
        g.sync();
        CHECK(find(g.run(), "display.mirror-unavailable")->severity == Severity::Info);
    }
    {   // an optional extra output that is simply unassigned is not a finding
        DisplayRouter r;
        REQUIRE(r.setOutputs({audience(1), [] { model::OutputDef o; o.id = 2; o.kind = RouteKind::Independent; o.label = "Extra"; return o; }()}).ok());
        REQUIRE(r.setTopology({D("L", "eDP-1", 1366, 768, true), D("P1", "h1")}).ok());
        REQUIRE(r.assignOutput(1, "P1", "h1").ok());
        EnvironmentSnapshot s; s.audio.deviceSelected = s.audio.deviceOpen = true; s.benchmark.run = true; s.benchmark.verdict = perf::Verdict::Pass;
        s.displays.routes = r.routes(); s.displays.displaysConnected = 2;
        CHECK(Doctor::run(s).empty());
    }
}

TEST_CASE("Doctor disk: unknown, too small and low margin for the planned recording") {
    Rig g;
    CHECK(g.run().empty());                                 // no recording planned => no disk check
    g.snap.disk.recordingPlanned = true; g.snap.disk.known = false;
    CHECK(find(g.run(), "disk.unknown")->severity == Severity::Warning);
    g.snap.disk.known = true; g.snap.disk.plannedBytes = 4ull << 30; g.snap.disk.freeBytes = 3ull << 30;
    auto f = g.run();
    CHECK(find(f, "disk.too-small")->severity == Severity::Blocker);
    CHECK(find(f, "disk.too-small")->message.find("4096 MiB") != std::string::npos);
    g.snap.disk.freeBytes = (4ull << 30) + (512ull << 20);
    CHECK(find(g.run(), "disk.low-margin")->severity == Severity::Warning);
    g.snap.disk.freeBytes = (4ull << 30) + (1ull << 30);     // exactly the margin is fine
    CHECK(g.run().empty());
    g.snap.disk.freeBytes = 4ull << 30;                      // exactly enough, but no margin
    CHECK(has(g.run(), "disk.low-margin"));
}

TEST_CASE("Doctor lighting: patch conflicts, unpatched fixtures, destination, network and armed state") {
    Rig g;
    CHECK(g.run().empty());                                  // lighting not configured => silent
    g.snap.lighting.configured = true;
    g.snap.lighting.patchConflicts = 1;
    auto f = g.run();
    CHECK(find(f, "lighting.patch-conflicts")->severity == Severity::Warning);   // disarmed: just a warning
    CHECK_FALSE(find(f, "lighting.patch-conflicts")->fix.has_value());
    g.snap.lighting.armed = true; g.snap.lighting.destinationSet = true;
    f = g.run();
    const Finding* c = find(f, "lighting.patch-conflicts");
    CHECK(c->severity == Severity::Blocker);                 // armed with conflicts
    REQUIRE(c->fix.has_value());
    CHECK(c->fix->action == ActionId::DisarmLighting);
    CHECK_FALSE(has(f, "lighting.armed"));                   // the blocker already covers the armed state
    g.snap.lighting.patchConflicts = 0; g.snap.lighting.unpatchedFixtures = 2;
    f = g.run();
    CHECK(find(f, "lighting.unpatched-fixtures")->message.find("2 fixtures") != std::string::npos);
    CHECK(find(f, "lighting.armed")->severity == Severity::Info);
    CHECK(find(f, "lighting.armed")->fix->action == ActionId::DisarmLighting);
    g.snap.lighting.destinationSet = false;
    CHECK(find(g.run(), "lighting.no-destination")->fix->action == ActionId::DisarmLighting);
    g.snap.lighting.destinationSet = true; g.snap.networkDisabled = true;
    f = g.run();
    CHECK(find(f, "lighting.network-disabled")->severity == Severity::Warning);
    CHECK(find(f, "network.offline")->severity == Severity::Info);
    g.snap.networkDisabled = false; g.snap.lighting.armed = false; g.snap.lighting.dryRun = true;
    f = g.run();
    CHECK(find(f, "lighting.dry-run")->severity == Severity::Info);
    CHECK_FALSE(has(f, "lighting.armed"));
}

TEST_CASE("Doctor midi: unavailable mappings are counted") {
    Rig g;
    g.snap.midi.mappingsTotal = 12; g.snap.midi.mappingsUnavailable = 0;
    CHECK(g.run().empty());
    g.snap.midi.mappingsUnavailable = 1;
    auto f = g.run();
    CHECK(find(f, "midi.mappings-unavailable")->severity == Severity::Warning);
    CHECK(find(f, "midi.mappings-unavailable")->message.find("1 MIDI mapping points") == 0);
    g.snap.midi.mappingsUnavailable = 3;
    CHECK(find(g.run(), "midi.mappings-unavailable")->message.find("3 MIDI mappings point") == 0);
    CHECK(find(g.run(), "midi.mappings-unavailable")->message.find("of 12") != std::string::npos);
}

TEST_CASE("Doctor media: missing and invalid media are warnings") {
    Rig g;
    g.snap.media.missing = 2;
    CHECK(find(g.run(), "media.missing")->message.find("2 media files are missing") == 0);
    g.snap.media.missing = 0; g.snap.media.invalid = 1;
    auto f = g.run();
    CHECK(find(f, "media.invalid")->severity == Severity::Warning);
    CHECK_FALSE(has(f, "media.missing"));
}

TEST_CASE("Doctor safety: strobing with flashing content offers the safe fix; disabled strobing is information") {
    Rig g;
    g.snap.safety.strobeContentPresent = false; g.snap.safety.strobingDisabled = false;
    CHECK(g.run().empty());                                   // nothing flashes => nothing to warn about
    g.snap.safety.strobeContentPresent = true;
    auto f = g.run();
    const Finding* x = find(f, "safety.strobe-enabled");
    REQUIRE(x);
    CHECK(x->severity == Severity::Warning);
    REQUIRE(x->fix.has_value());
    CHECK(x->fix->action == ActionId::DisableStrobing);
    g.snap.safety.strobingDisabled = true;
    f = g.run();
    CHECK_FALSE(has(f, "safety.strobe-enabled"));
    CHECK(find(f, "safety.strobe-disabled")->severity == Severity::Info);
    g.snap.safety.visualBlackoutLatched = true; g.snap.safety.lightingBlackoutLatched = true;
    f = g.run();
    CHECK(find(f, "safety.visual-blackout")->severity == Severity::Warning);
    CHECK_FALSE(find(f, "safety.visual-blackout")->fix.has_value());          // restoring a blackout is the operator's call
    CHECK(find(f, "safety.lighting-blackout")->severity == Severity::Warning);
}

TEST_CASE("Doctor benchmark: never run, stale, changed setup, failed and per-feature admission") {
    Rig g;
    g.snap.benchmark = {};
    auto f = g.run();
    CHECK(find(f, "benchmark.never-run")->severity == Severity::Warning);
    g.snap.benchmark.run = true; g.snap.benchmark.verdict = perf::Verdict::Pass; g.snap.benchmark.ageMs = 31LL * 24 * 3600 * 1000;
    f = g.run();
    CHECK(find(f, "benchmark.stale")->message.find("31 days") != std::string::npos);
    DoctorOptions o; o.benchmarkMaxAgeMs = 60LL * 24 * 3600 * 1000;
    CHECK_FALSE(has(Doctor::run(g.snap, o), "benchmark.stale"));
    g.snap.benchmark.ageMs = 0; g.snap.benchmark.matchesConfig = false;
    CHECK(has(g.run(), "benchmark.config-changed"));
    CHECK_FALSE(has(g.run(), "benchmark.stale"));            // a mismatched profile is reported once, as changed
    g.snap.benchmark.matchesConfig = true; g.snap.benchmark.verdict = perf::Verdict::Fail;
    CHECK(find(g.run(), "benchmark.failed")->severity == Severity::Warning);
    g.snap.benchmark.verdict = perf::Verdict::Incomplete;
    CHECK(find(g.run(), "benchmark.incomplete")->severity == Severity::Info);
    g.snap.benchmark.verdict = perf::Verdict::Pass;
    g.snap.benchmark.features = {
        {"camera", perf::AdmissionState::Rejected, true, "needs about 10% CPU but only 4% headroom is left"},
        {"av-record", perf::AdmissionState::Untested, true, "untested, run benchmark BEN-04"},
        {"second-output", perf::AdmissionState::AdmittedByOverride, true, ""},
        {"gif", perf::AdmissionState::Rejected, false, "unused by this show"},       // not used => silent
        {"text", perf::AdmissionState::Admitted, true, ""}};                          // admitted => silent
    f = g.run();
    CHECK(find(f, "benchmark.feature-rejected.camera")->severity == Severity::Warning);
    CHECK(find(f, "benchmark.feature-rejected.camera")->message.find("headroom") != std::string::npos);
    CHECK(find(f, "benchmark.feature-untested.av-record")->severity == Severity::Warning);
    CHECK(find(f, "benchmark.feature-override.second-output")->severity == Severity::Info);
    CHECK_FALSE(has(f, "benchmark.feature-rejected.gif"));
    CHECK(f.size() == 3);
}

TEST_CASE("Doctor broadcast: missing secret reference and offline mode block going live") {
    Rig g;
    g.snap.broadcast.configured = false;
    CHECK(g.run().empty());
    g.snap.broadcast.configured = true; g.snap.broadcast.secretReferencePresent = false;
    auto f = g.run();
    CHECK(find(f, "broadcast.no-secret")->severity == Severity::Blocker);
    CHECK(find(f, "broadcast.no-secret")->message.find("key") != std::string::npos);
    g.snap.broadcast.secretReferencePresent = true;
    CHECK(g.run().empty());
    g.snap.networkDisabled = true;
    f = g.run();
    CHECK(find(f, "broadcast.network-disabled")->severity == Severity::Blocker);
    CHECK(find(f, "network.offline")->severity == Severity::Info);
}

TEST_CASE("Doctor show validation: errors block, warnings inform, messages are clipped") {
    Rig g;
    g.snap.validation.warnings = 2;
    auto f = g.run();
    CHECK(find(f, "show.warnings")->severity == Severity::Warning);
    g.snap.validation.errors = 1;
    g.snap.validation.firstMessages = {std::string(500, 'x'), "second", "third", "fourth"};
    f = g.run();
    const Finding* x = find(f, "show.invalid");
    REQUIRE(x);
    CHECK(x->severity == Severity::Blocker);
    CHECK_FALSE(has(f, "show.warnings"));                    // errors take precedence in one finding
    CHECK(x->message.size() < 400);
    CHECK(x->message.find("third") != std::string::npos);
    CHECK(x->message.find("fourth") == std::string::npos);
    g.snap.validation.firstMessages.clear();
    CHECK(find(g.run(), "show.invalid")->message.find("1 problem that must") != std::string::npos);
}

TEST_CASE("Doctor: a finding never offers an unsafe fix and never involves transmitting actions") {
    // Property test over random snapshots: every fix is a registered safe action, no check carries a
    // StartRecording/StartBroadcast/ArmLighting/EnableStrobing/RestoreBlackout fix.
    Rng rng(777);
    for (int i = 0; i < 400; ++i) {
        EnvironmentSnapshot s;
        auto b = [&] { return rng.below(2) != 0; };
        s.audio = {b(), b(), b(), b(), b(), static_cast<double>(rng.below(40)), b(), rng.below(5)};
        s.disk = {b(), static_cast<std::uint64_t>(rng.below(8)) << 30, b(), static_cast<std::uint64_t>(rng.below(8)) << 30};
        s.lighting = {b(), b(), b(), rng.below(3), rng.below(3), b()};
        s.midi = {rng.below(10), rng.below(10)};
        s.media = {rng.below(3), rng.below(3)};
        s.safety = {b(), b(), b(), b()};
        s.benchmark.run = b(); s.benchmark.matchesConfig = b(); s.benchmark.ageMs = static_cast<std::int64_t>(rng.below(60)) * 24 * 3600 * 1000;
        s.benchmark.verdict = static_cast<perf::Verdict>(rng.below(3));
        s.broadcast = {b(), b()};
        s.validation.errors = rng.below(2); s.validation.warnings = rng.below(2);
        s.networkDisabled = b();
        auto f = Doctor::run(s);
        for (const Finding& x : f) {
            if (!x.fix) continue;
            INFO(x.id);
            CHECK(Doctor::isSafeFixAction(x.fix->action));
            CHECK(x.fix->action != ActionId::StartRecording);
            CHECK(x.fix->action != ActionId::StartBroadcast);
            CHECK(x.fix->action != ActionId::ArmLighting);
            CHECK(x.fix->action != ActionId::EnableStrobing);
            CHECK(x.fix->action != ActionId::RestoreBlackout);
        }
        for (std::size_t k = 1; k < f.size(); ++k) CHECK(static_cast<int>(f[k - 1].severity) <= static_cast<int>(f[k].severity));
    }
    CHECK(Doctor::isSafeFixAction(ActionId::DisarmLighting));
    CHECK(Doctor::isSafeFixAction(ActionId::DisableStrobing));
    CHECK(Doctor::isSafeFixAction(ActionId::IdentifyDisplays));
    CHECK_FALSE(Doctor::isSafeFixAction(ActionId::StartBroadcast));
    CHECK_FALSE(Doctor::isSafeFixAction(ActionId::None));
}

TEST_CASE("Doctor: run is a pure function of const input (no side channel to transmit anything)") {
    // The signature takes only a const snapshot (and const options): no callbacks, clocks, sockets or files.
    static_assert(std::is_same<decltype(Doctor::run(std::declval<const EnvironmentSnapshot&>())), std::vector<Finding>>::value, "pure signature");
    static_assert(std::is_same<decltype(Doctor::run(std::declval<const EnvironmentSnapshot&>(), std::declval<const DoctorOptions&>())), std::vector<Finding>>::value, "pure signature");
    EnvironmentSnapshot s;
    s.broadcast.configured = true; s.lighting.configured = s.lighting.armed = true; s.networkDisabled = false;
    const EnvironmentSnapshot copy = s;
    auto a = Doctor::run(s);
    CHECK(s.broadcast.configured == copy.broadcast.configured);
    CHECK(s.lighting.armed == copy.lighting.armed);
    CHECK(Doctor::run(s).size() == a.size());
    // Secrets and paths cannot appear: the snapshot has no field that could carry them, and the broadcast
    // check only sees a boolean "secret reference present".
    for (const Finding& x : a) {
        CHECK(x.message.find("rtmp") == std::string::npos);
        CHECK(x.message.find("key=") == std::string::npos);
    }
}

TEST_CASE("Doctor: findings change when the environment is repaired (one-click fix loop)") {
    Rig g;
    g.snap.safety.strobeContentPresent = true;
    g.snap.lighting.configured = g.snap.lighting.armed = g.snap.lighting.destinationSet = true;
    g.snap.lighting.patchConflicts = 1;
    auto f = g.run();
    REQUIRE(Doctor::hasBlockers(f));
    for (const Finding& x : f) {
        if (!x.fix) continue;
        if (x.fix->action == ActionId::DisableStrobing) g.snap.safety.strobingDisabled = true;
        if (x.fix->action == ActionId::DisarmLighting) g.snap.lighting.armed = false;
    }
    f = g.run();
    CHECK_FALSE(Doctor::hasBlockers(f));
    CHECK_FALSE(has(f, "safety.strobe-enabled"));
    CHECK(has(f, "lighting.patch-conflicts"));               // still a warning: the patch itself is unchanged
}
