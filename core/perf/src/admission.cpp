#include "ss/perf/admission.hpp"
#include <algorithm>
#include <cstdio>

namespace ss::perf {

const char* admissionStateName(AdmissionState s) {
    switch (s) {
        case AdmissionState::Admitted: return "admitted";
        case AdmissionState::AdmittedByOverride: return "admitted-by-override";
        case AdmissionState::Rejected: return "rejected";
        case AdmissionState::Untested: return "untested";
    }
    return "rejected";
}

const ProfileScenario* BenchmarkProfile::find(const std::string& id) const {
    for (const ProfileScenario& s : scenarios) if (s.id == id) return &s;
    return nullptr;
}

BenchmarkProfile profileFromReport(const SuiteReport& r) {
    BenchmarkProfile p;
    p.present = !r.scenarios.empty();
    p.profileHash = r.profileHash();
    p.capturedAtUnixMs = r.capturedAtUnixMs;
    for (const ScenarioReport& s : r.scenarios) {
        ProfileScenario ps;
        ps.id = s.scenarioId;
        ps.verdict = s.gatesWorst.verdict;
        ps.metricsAvailable = s.median.available("fps.mean");
        ps.fps = s.median.value("fps.mean");
        ps.p95FrameMs = s.median.value("interval.p95.ms");
        ps.cpuPct = s.median.value("cpu.process.avg");
        ps.workingSetBytes = static_cast<std::uint64_t>(s.median.value("mem.ws.peak"));
        p.scenarios.push_back(ps);
    }
    return p;
}

namespace {
std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}
bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
}  // namespace

AdmissionResult admitFeatures(const AdmissionInput& in) {
    AdmissionResult out;
    CapabilityStatus& st = out.status;
    st.profileHash = in.currentProfileHash;
    st.adaptationStep = in.adaptationStep;

    // Profile validity: matching hash and recent enough.
    std::string profileProblem;
    if (!in.profile || !in.profile->present) profileProblem = "no benchmark has been run on this configuration";
    else if (in.profile->profileHash != in.currentProfileHash) profileProblem = "the last benchmark was for a different configuration";
    else if (in.nowUnixMs - in.profile->capturedAtUnixMs > in.maxProfileAgeMs) profileProblem = "the last benchmark is too old";
    else if (in.profile->capturedAtUnixMs > in.nowUnixMs + 24LL * 3600 * 1000) profileProblem = "the last benchmark has an invalid timestamp";
    const bool profileOk = profileProblem.empty();

    // Baseline = BEN-01 (basic profile) when present, else the cheapest scenario that has metrics.
    const ProfileScenario* base = nullptr;
    if (profileOk) {
        base = in.profile->find("BEN-01");
        if (!base || !base->metricsAvailable) {
            base = nullptr;
            for (const ProfileScenario& s : in.profile->scenarios)
                if (s.metricsAvailable && (!base || s.cpuPct < base->cpuPct)) base = &s;
        }
    }
    out.baselineKnown = base != nullptr;
    double cpuLeft = 0;
    std::int64_t memLeft = 0;
    if (base) {
        cpuLeft = in.cpuBudgetPct - in.cpuMarginPct - base->cpuPct;
        memLeft = static_cast<std::int64_t>(in.memoryBudgetBytes) - static_cast<std::int64_t>(base->workingSetBytes);
        st.metricsAvailable = true;
        st.fps = base->fps; st.p95FrameMs = base->p95FrameMs; st.cpuPct = base->cpuPct; st.workingSetBytes = base->workingSetBytes;
    }
    out.cpuHeadroomPct = cpuLeft;
    out.memoryHeadroomBytes = memLeft > 0 ? static_cast<std::uint64_t>(memLeft) : 0;

    for (const FeatureDef& f : in.requested) {
        FeatureDecision d;
        d.id = f.id;
        bool hard = false;                      // a hard rejection cannot be overridden
        std::vector<std::string> soft;
        bool allUntested = true;                // every soft reason is an "untested" one
        auto addSoft = [&](const std::string& text, bool untestedKind) {
            soft.push_back(text);
            if (!untestedKind) allUntested = false;
        };

        if (static_cast<int>(f.minTier) > static_cast<int>(in.tier)) {
            hard = true;
            d.reasons.push_back(std::string("needs ") + tierName(f.minTier) + " hardware; this machine is " + tierName(in.tier));
        }
        for (const std::string& dev : f.requiredDevices)
            if (!has(in.availableDevices, dev)) { hard = true; d.reasons.push_back("required device not available: " + dev); }
        if (f.needsEncoder && !in.encoderAvailable) { hard = true; d.reasons.push_back("no encoder available"); }
        if (in.qualityLocked && in.overloaded) {
            hard = true;
            d.reasons.push_back("quality is locked and the system is overloaded; added work refused");
        }

        if (!profileOk) addSoft("untested, run benchmark (" + profileProblem + ")", true);
        else if (!f.requiredScenario.empty()) {
            const ProfileScenario* ps = in.profile->find(f.requiredScenario);
            if (!ps) addSoft("untested, run benchmark " + f.requiredScenario, true);
            else if (ps->verdict == Verdict::Fail) addSoft("benchmark " + f.requiredScenario + " did not pass its gates", false);
            else if (ps->verdict == Verdict::Incomplete) addSoft("untested, benchmark " + f.requiredScenario + " is incomplete (counters missing)", true);
        }
        if (profileOk && !base) addSoft("untested, benchmark has no usable CPU/memory measurements", true);

        // Budget: only meaningful with a baseline; counted against already admitted features.
        if (base) {
            if (f.cpuCostPct > cpuLeft + 1e-9)
                addSoft("needs about " + num(f.cpuCostPct) + "% CPU but only " + num(std::max(0.0, cpuLeft)) + "% headroom is left", false);
            if (static_cast<std::int64_t>(f.memoryBytes) > memLeft)
                addSoft("needs about " + std::to_string(f.memoryBytes / kMiB) + " MiB but only " +
                        std::to_string(static_cast<std::uint64_t>(std::max<std::int64_t>(0, memLeft)) / kMiB) +
                        " MiB of the memory budget is left", false);
        }

        const AdmissionOverride* ov = nullptr;
        for (const AdmissionOverride& o : in.overrides) if (o.featureId == f.id) { ov = &o; break; }
        const bool ovValid = ov && !ov->reason.empty();

        if (hard) {
            d.state = AdmissionState::Rejected;
            for (const auto& r : soft) d.reasons.push_back(r);
        } else if (soft.empty()) {
            d.state = AdmissionState::Admitted;
        } else if (ovValid) {
            d.state = AdmissionState::AdmittedByOverride;
            d.reasons = soft;
            d.reasons.push_back("operator override: " + ov->reason);
        } else {
            d.state = allUntested ? AdmissionState::Untested : AdmissionState::Rejected;
            d.reasons = soft;
            d.overridable = true;
            if (ov) d.reasons.push_back("override ignored: a reason is required");
        }

        if (d.state == AdmissionState::Admitted || d.state == AdmissionState::AdmittedByOverride) {
            cpuLeft -= f.cpuCostPct;
            memLeft -= static_cast<std::int64_t>(f.memoryBytes);
            st.admitted.push_back(f.id);
        } else {
            st.rejected.push_back(f.id);
        }
        for (const std::string& r : d.reasons) st.reasons.push_back(f.id + ": " + r);
        out.decisions.push_back(std::move(d));
    }
    return out;
}

// ---- soak ----------------------------------------------------------------------------------
void SoakMonitor::add(const SoakSample& s) {
    if (s_.size() >= cfg_.maxSamples) { truncated_ = true; return; }
    if (!s_.empty() && s.t < s_.back().t) return;       // time never goes backwards; ignore bad samples
    s_.push_back(s);
}

namespace {
double meanIn(const std::vector<SoakSample>& v, Ns from, Ns to, bool commit, bool* any) {
    double sum = 0; std::size_t n = 0;
    for (const SoakSample& s : v) {
        if (s.t >= from && s.t <= to) { sum += static_cast<double>(commit ? s.privateCommitBytes : s.workingSetBytes); ++n; }
    }
    *any = n > 0;
    return n ? sum / static_cast<double>(n) : 0.0;
}
}  // namespace

SoakReport SoakMonitor::evaluate() const {
    SoakReport rep;
    rep.truncated = truncated_;
    auto add = [&](const char* id, const char* desc, GateState st, double measured, double limit, const char* unit) {
        GateResult g; g.id = id; g.description = desc; g.state = st; g.measured = measured; g.limit = limit;
        g.unit = unit; g.upperBound = true; g.req = "REQ-X06";
        rep.checks.push_back(g);
    };
    if (s_.size() < 2) {
        add("S-DURATION", "Soak ran for the full duration", GateState::Unavailable, 0, nsToSec(cfg_.duration), "s");
        rep.verdict = Verdict::Incomplete;
        return rep;
    }
    const Ns t0 = s_.front().t, t1 = s_.back().t, span = t1 - t0;
    add("S-DURATION", "Soak ran for the full duration", span >= cfg_.duration ? GateState::Pass : GateState::Unavailable,
        nsToSec(span), nsToSec(cfg_.duration), "s");
    add("S-CRASH", "No crash during the soak", crashes_ == 0 ? GateState::Pass : GateState::Fail, crashes_, 0, "count");

    // Unbounded growth: mean of [10%,20%] vs mean of [90%,100%] of the run, for working set and commit.
    for (int commit = 0; commit < 2; ++commit) {
        bool a = false, b = false;
        const double early = meanIn(s_, t0 + span / 10, t0 + span / 5, commit != 0, &a);
        const double late = meanIn(s_, t0 + span * 9 / 10, t1, commit != 0, &b);
        const char* id = commit ? "S-COMMIT-GROWTH" : "S-WS-GROWTH";
        const char* desc = commit ? "Private commit does not grow without bound" : "Working set does not grow without bound";
        if (!a || !b) { add(id, desc, GateState::Unavailable, 0, 0, "bytes"); continue; }
        const double growth = late - early;
        const double allowed = std::max(static_cast<double>(cfg_.maxGrowthBytes), early * cfg_.maxGrowthFraction);
        add(id, desc, growth <= allowed ? GateState::Pass : GateState::Fail, growth, allowed, "bytes");
    }
    std::uint64_t peakWs = 0, peakCommit = 0;
    for (const SoakSample& s : s_) { peakWs = std::max(peakWs, s.workingSetBytes); peakCommit = std::max(peakCommit, s.privateCommitBytes); }
    add("S-PEAK", "Working set and commit stay under the peak cap",
        std::max(peakWs, peakCommit) <= cfg_.peakCapBytes ? GateState::Pass : GateState::Fail,
        static_cast<double>(std::max(peakWs, peakCommit)), static_cast<double>(cfg_.peakCapBytes), "bytes");

    // Stuck blackout: active but never requested, for longer than the allowance.
    Ns stuckSince = -1, worst = 0;
    for (const SoakSample& s : s_) {
        if (s.blackoutActive && !s.blackoutRequested) { if (stuckSince < 0) stuckSince = s.t; worst = std::max(worst, s.t - stuckSince); }
        else stuckSince = -1;
    }
    add("S-BLACKOUT", "No stuck blackout", worst <= cfg_.stuckBlackoutNs ? GateState::Pass : GateState::Fail, nsToSec(worst),
        nsToSec(cfg_.stuckBlackoutNs), "s");

    const SoakSample& last = s_.back();
    const std::uint64_t undeclared = last.recordingFramesLost > last.recordingLossDeclared ? last.recordingFramesLost - last.recordingLossDeclared : 0;
    add("S-REC-LOSS", "No undeclared recording loss", undeclared == 0 ? GateState::Pass : GateState::Fail, static_cast<double>(undeclared), 0, "frames");
    add("S-CAPTURE-OVERRUNS", "Zero capture overruns", last.captureOverruns == 0 ? GateState::Pass : GateState::Fail,
        static_cast<double>(last.captureOverruns), 0, "count");

    int fail = 0, unav = 0;
    for (const GateResult& g : rep.checks) { if (g.state == GateState::Fail) ++fail; else if (g.state == GateState::Unavailable) ++unav; }
    rep.verdict = fail ? Verdict::Fail : (unav ? Verdict::Incomplete : Verdict::Pass);
    return rep;
}

}  // namespace ss::perf
