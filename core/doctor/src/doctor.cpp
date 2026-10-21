#include "ss/doctor/doctor.hpp"
#include <algorithm>

namespace ss::doctor {

using router::RouteStatus;

const char* severityName(Severity s) {
    switch (s) {
        case Severity::Blocker: return "blocker";
        case Severity::Warning: return "warning";
        case Severity::Info: return "info";
    }
    return "info";
}

bool Doctor::isSafeFixAction(ActionId a) {
    // Fixes may only reduce risk or help the operator look: never start recording/broadcast/lighting,
    // never lift a blackout and never enable strobing.
    return a == ActionId::DisarmLighting || a == ActionId::DisableStrobing || a == ActionId::IdentifyDisplays;
}

bool Doctor::hasBlockers(const std::vector<Finding>& f) {
    for (const Finding& x : f) if (x.severity == Severity::Blocker) return true;
    return false;
}

namespace {

struct Out {
    std::vector<Finding> v;
    void add(Severity s, std::string id, std::string msg, std::string why) {
        Finding f; f.severity = s; f.id = std::move(id); f.message = std::move(msg); f.why = std::move(why);
        v.push_back(std::move(f));
    }
    void addFix(Severity s, std::string id, std::string msg, std::string why, ActionId a, double value, std::string label) {
        add(s, std::move(id), std::move(msg), std::move(why));
        v.back().fix = Fix{a, value, std::move(label)};
    }
};

std::string clip(const std::string& s, std::size_t n) { return s.size() > n ? s.substr(0, n) + "..." : s; }
std::string plural(std::uint64_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }
std::string mib(std::uint64_t b) { return std::to_string(b / (1024ull * 1024ull)) + " MiB"; }

void checkAudio(const EnvironmentSnapshot& e, const DoctorOptions& o, Out& out) {
    const AudioFacts& a = e.audio;
    if (a.permissionDenied)
        out.add(Severity::Blocker, "audio.permission-denied", "StageSignal is not allowed to use the audio input.",
                "Without audio the visuals and lights cannot follow the music.");
    else if (!a.deviceSelected)
        out.add(Severity::Blocker, "audio.no-device", "No audio input is selected.",
                "The visuals and lights react to the music, so they need an input.");
    else if (a.deviceLost)
        out.add(Severity::Blocker, "audio.device-lost", "The selected audio input was unplugged or stopped.",
                "The show will keep running but will not react to the music until an input is back.");
    else if (a.formatUnsupported)
        out.add(Severity::Blocker, "audio.format-unsupported", "The selected audio input uses a format StageSignal cannot read.",
                "Pick another input or sample rate so the music can be analysed.");
    else if (!a.deviceOpen)
        out.add(Severity::Blocker, "audio.not-open", "The audio input is selected but not running.",
                "Nothing is listening to the music yet.");
    if (a.deviceSelected && a.deviceOpen && !a.deviceLost && a.silentSeconds >= o.silenceWarnSeconds)
        out.add(Severity::Warning, "audio.silent", "The audio input has been silent for " + std::to_string(static_cast<long long>(a.silentSeconds)) + " seconds.",
                "Check the cable, the mixer send and the volume; otherwise the show cannot follow the music.");
    if (a.deviceSelected && a.deviceOpen && !a.deviceLost && a.clipping)
        out.add(Severity::Warning, "audio.clipping", "The audio input is hitting its maximum level.",
                "A clipped signal makes tempo and key detection less reliable; turn the input level down at the source.");
}

void checkDisplays(const EnvironmentSnapshot& e, Out& out) {
    const DisplayFacts& d = e.displays;
    bool singleDisplayReported = false;
    for (const router::RouteInfo& r : d.routes) {
        if (r.isControls || r.isMirror) {
            if (r.isMirror && (r.status == RouteStatus::Disconnected || r.status == RouteStatus::NeedsConfirmation))
                out.add(Severity::Info, "display.mirror-unavailable", "The second controls screen is not available.",
                        "The main controls are unaffected.");
            continue;
        }
        const std::string tag = "." + std::to_string(r.route.logicalId);
        const bool audience = r.route.kind == RouteKind::Audience;
        const std::string name = "Output \"" + clip(r.label, 40) + "\"";
        switch (r.status) {
            case RouteStatus::Disconnected:
                out.addFix(audience ? Severity::Blocker : Severity::Warning, "display.disconnected" + tag,
                           name + " has lost its display.", "Nothing is showing there. The controls were not moved onto it.",
                           ActionId::IdentifyDisplays, 0, "Identify displays");
                break;
            case RouteStatus::NeedsConfirmation:
                out.addFix(audience ? Severity::Blocker : Severity::Warning, "display.needs-confirmation" + tag,
                           name + " needs you to confirm which display to use.",
                           "It stays blank until confirmed, so the wrong screen is never lit by mistake.",
                           ActionId::IdentifyDisplays, 0, "Identify displays");
                break;
            case RouteStatus::Suspended:
                out.addFix(audience ? Severity::Blocker : Severity::Warning, "display.suspended" + tag,
                           name + " is paused because the controls are using its display.",
                           "The audience sees nothing until the controls display is available again.",
                           ActionId::IdentifyDisplays, 0, "Identify displays");
                break;
            case RouteStatus::Unassigned:
                if (!audience) break;       // optional extra outputs may stay unassigned
                if (d.displaysConnected <= 1) {
                    if (!singleDisplayReported) {
                        singleDisplayReported = true;
                        out.add(Severity::Warning, "display.single-display", "Only one display is connected, so there is no separate audience screen.",
                                "The audience cannot see the visuals on a different screen from your controls.");
                    }
                } else {
                    out.addFix(Severity::Blocker, "display.unassigned" + tag, name + " has no display assigned.",
                               "The audience will see nothing.", ActionId::IdentifyDisplays, 0, "Identify displays");
                }
                break;
            case RouteStatus::Disabled:
                out.add(Severity::Warning, "display.disabled" + tag, name + " is switched off: " + clip(r.statusText, 160),
                        "This machine's measured capability does not allow it, so it will stay blank.");
                break;
            case RouteStatus::Active:
                if (r.route.showDiagnostics)
                    out.add(Severity::Warning, "display.audience-diagnostics" + tag, name + " is showing diagnostics to the audience.",
                            "Technical overlays on the audience screen look unprofessional and can leak details.");
                break;
        }
    }
    if (d.controlsOnFallback)
        out.add(Severity::Warning, "display.controls-fallback", "Your controls moved to another display because their own display is not available.",
                "Some audience output may be paused so it never covers the controls.");
}

void checkDisk(const EnvironmentSnapshot& e, const DoctorOptions& o, Out& out) {
    const DiskFacts& d = e.disk;
    if (!d.recordingPlanned) return;
    if (!d.known) {
        out.add(Severity::Warning, "disk.unknown", "Free disk space could not be read for the planned recording.",
                "The recording might stop part-way if the disk fills up.");
        return;
    }
    if (d.plannedBytes > d.freeBytes)
        out.add(Severity::Blocker, "disk.too-small", "The planned recording needs about " + mib(d.plannedBytes) + " but only " + mib(d.freeBytes) + " is free.",
                "The recording would stop when the disk is full.");
    else if (d.freeBytes - d.plannedBytes < o.diskMarginBytes)
        out.add(Severity::Warning, "disk.low-margin", "The planned recording leaves less than " + mib(o.diskMarginBytes) + " free on the disk.",
                "A nearly full disk can slow the whole computer during the show.");
}

void checkLighting(const EnvironmentSnapshot& e, Out& out) {
    const LightingFacts& l = e.lighting;
    if (!l.configured) return;
    if (l.patchConflicts > 0) {
        const std::string msg = plural(l.patchConflicts, "fixture patch overlaps", "fixture patches overlap") + " another one.";
        const std::string why = "Overlapping DMX addresses make fixtures react to the wrong channels.";
        if (l.armed) out.addFix(Severity::Blocker, "lighting.patch-conflicts", msg, why, ActionId::DisarmLighting, 0, "Disarm lighting");
        else out.add(Severity::Warning, "lighting.patch-conflicts", msg, why);
    }
    if (l.unpatchedFixtures > 0)
        out.add(Severity::Warning, "lighting.unpatched-fixtures", plural(l.unpatchedFixtures, "fixture used by the show is", "fixtures used by the show are") + " not patched.",
                "Unpatched fixtures stay dark during the show.");
    if (l.armed && !l.destinationSet)
        out.addFix(Severity::Warning, "lighting.no-destination", "Lighting is armed but has no destination address.",
                   "No DMX can reach the fixtures.", ActionId::DisarmLighting, 0, "Disarm lighting");
    if (l.armed && e.networkDisabled)
        out.addFix(Severity::Warning, "lighting.network-disabled", "Lighting is armed but the network is disabled.",
                   "Art-Net and sACN cannot be sent while the network is off.", ActionId::DisarmLighting, 0, "Disarm lighting");
    if (l.armed && l.patchConflicts == 0)
        out.addFix(Severity::Info, "lighting.armed", "Lighting is armed: fixtures will follow the show.",
                   "Make sure the stage is clear and fixtures are safe to move or flash.", ActionId::DisarmLighting, 0, "Disarm lighting");
    if (l.dryRun)
        out.add(Severity::Info, "lighting.dry-run", "Lighting is in dry-run mode: nothing is sent to fixtures.",
                "Turn dry-run off before the show if you expect lights.");
}

void checkMidi(const EnvironmentSnapshot& e, Out& out) {
    if (e.midi.mappingsUnavailable == 0) return;
    out.add(Severity::Warning, "midi.mappings-unavailable",
            plural(e.midi.mappingsUnavailable, "MIDI mapping points", "MIDI mappings point") + " at a controller that is not connected" +
                (e.midi.mappingsTotal ? " (of " + std::to_string(e.midi.mappingsTotal) + ")." : "."),
            "Those pads and knobs will do nothing until the controller is plugged in.");
}

void checkMedia(const EnvironmentSnapshot& e, Out& out) {
    if (e.media.missing > 0)
        out.add(Severity::Warning, "media.missing", plural(e.media.missing, "media file is", "media files are") + " missing.",
                "Scenes that use them will show a placeholder instead of your content.");
    if (e.media.invalid > 0)
        out.add(Severity::Warning, "media.invalid", plural(e.media.invalid, "media file cannot", "media files cannot") + " be used (damaged or changed).",
                "Scenes that use them will show a placeholder instead of your content.");
}

void checkSafety(const EnvironmentSnapshot& e, Out& out) {
    const SafetyFacts& s = e.safety;
    if (s.strobeContentPresent && !s.strobingDisabled)
        out.addFix(Severity::Warning, "safety.strobe-enabled", "Strobing is allowed and the show contains flashing content.",
                   "Flashing can be a health risk for some audience members; disable it unless the venue has agreed.",
                   ActionId::DisableStrobing, 0, "Disable strobing");
    if (s.strobeContentPresent && s.strobingDisabled)
        out.add(Severity::Info, "safety.strobe-disabled", "Strobing is disabled; flashing content will be softened.",
                "This is the safe setting for most venues.");
    if (s.visualBlackoutLatched)
        out.add(Severity::Warning, "safety.visual-blackout", "Visual blackout is on.",
                "The audience sees black until you restore it.");
    if (s.lightingBlackoutLatched)
        out.add(Severity::Warning, "safety.lighting-blackout", "Lighting blackout is on.",
                "The fixtures stay dark until you restore it.");
}

void checkBenchmark(const EnvironmentSnapshot& e, const DoctorOptions& o, Out& out) {
    const BenchmarkFacts& b = e.benchmark;
    if (!b.run) {
        out.add(Severity::Warning, "benchmark.never-run", "No benchmark has been run on this computer.",
                "StageSignal cannot tell which extra features this computer can handle until it has been measured.");
        return;
    }
    if (!b.matchesConfig)
        out.add(Severity::Warning, "benchmark.config-changed", "The last benchmark was for a different setup (devices, displays or settings changed).",
                "Its results may not apply to the setup you will perform with.");
    else if (b.ageMs > o.benchmarkMaxAgeMs)
        out.add(Severity::Warning, "benchmark.stale", "The last benchmark is " + std::to_string(b.ageMs / (24LL * 3600 * 1000)) + " days old.",
                "Drivers and system load change; re-run it before an important show.");
    if (b.verdict == perf::Verdict::Fail)
        out.add(Severity::Warning, "benchmark.failed", "The last benchmark did not meet the performance targets.",
                "Expect dropped frames; keep the show simple or lower quality.");
    else if (b.verdict == perf::Verdict::Incomplete)
        out.add(Severity::Info, "benchmark.incomplete", "The last benchmark could not measure everything.",
                "Some limits are unknown, so some features stay untested.");
    for (const FeatureAdmission& f : b.features) {
        if (!f.usedByShow) continue;
        const std::string reason = f.reason.empty() ? std::string() : " (" + clip(f.reason, 120) + ")";
        if (f.state == perf::AdmissionState::Rejected)
            out.add(Severity::Warning, "benchmark.feature-rejected." + f.id, "\"" + clip(f.id, 40) + "\" is used by the show but this computer is not cleared to run it" + reason + ".",
                    "It is likely to drop frames or stay off during the show.");
        else if (f.state == perf::AdmissionState::Untested)
            out.add(Severity::Warning, "benchmark.feature-untested." + f.id, "\"" + clip(f.id, 40) + "\" is used by the show but is untested; run the benchmark" + reason + ".",
                    "Nobody knows yet whether this computer can run it smoothly.");
        else if (f.state == perf::AdmissionState::AdmittedByOverride)
            out.add(Severity::Info, "benchmark.feature-override." + f.id, "\"" + clip(f.id, 40) + "\" is running on a manual override.",
                    "It was not cleared by a benchmark, so watch the performance strip.");
    }
}

void checkBroadcast(const EnvironmentSnapshot& e, Out& out) {
    if (e.networkDisabled)
        out.add(Severity::Info, "network.offline", "Network is disabled: nothing leaves this computer.",
                "Broadcast and network lighting will not work in this mode.");
    if (!e.broadcast.configured) return;
    if (!e.broadcast.secretReferencePresent)
        out.add(Severity::Blocker, "broadcast.no-secret", "Broadcast is set up but the stream key is missing.",
                "You cannot go live without it. Enter the key in the broadcast settings.");
    if (e.networkDisabled)
        out.add(Severity::Blocker, "broadcast.network-disabled", "Broadcast is set up but the network is disabled.",
                "You cannot go live while the network is off.");
}

void checkValidation(const EnvironmentSnapshot& e, Out& out) {
    const ValidationFacts& v = e.validation;
    std::string detail;
    for (std::size_t i = 0; i < v.firstMessages.size() && i < 3; ++i) detail += (i ? "; " : " ") + clip(v.firstMessages[i], 100);
    if (v.errors > 0)
        out.add(Severity::Blocker, "show.invalid", "The show has " + plural(v.errors, "problem", "problems") + " that must be fixed:" + (detail.empty() ? " see the show checks." : detail),
                "The show may not load or may behave unpredictably on stage.");
    else if (v.warnings > 0)
        out.add(Severity::Warning, "show.warnings", "The show has " + plural(v.warnings, "warning", "warnings") + "." + (detail.empty() ? "" : detail),
                "These are unlikely to stop the show but are worth a look.");
}

}  // namespace

std::vector<Finding> Doctor::run(const EnvironmentSnapshot& snapshot) { return run(snapshot, DoctorOptions{}); }

std::vector<Finding> Doctor::run(const EnvironmentSnapshot& e, const DoctorOptions& o) {
    Out out;
    checkAudio(e, o, out);
    checkDisplays(e, out);
    checkDisk(e, o, out);
    checkLighting(e, out);
    checkMidi(e, out);
    checkMedia(e, out);
    checkSafety(e, out);
    checkBenchmark(e, o, out);
    checkBroadcast(e, out);
    checkValidation(e, out);
    std::stable_sort(out.v.begin(), out.v.end(), [](const Finding& a, const Finding& b) {
        if (a.severity != b.severity) return static_cast<int>(a.severity) < static_cast<int>(b.severity);
        return a.id < b.id;
    });
    return std::move(out.v);
}

}  // namespace ss::doctor
