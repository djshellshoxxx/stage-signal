#include "perf/admission.h"
#include "nlohmann/json.hpp"
#include <algorithm>

namespace ss {

const char* admissionName(Admission a) { static const char* n[] = {"admitted", "untested", "denied", "overridden"}; return (size_t)a < 4 ? n[(size_t)a] : "?"; }

std::vector<FeatureDef> defaultFeatureCatalog() {
  auto mk = [](const char* id, const char* label, double cpu, uint64_t mem, bool enc, bool bench, bool req, BenchId sc, const char* fb) {
    FeatureDef f; f.id = id; f.label = label; f.cpuCostPct = cpu; f.memBytes = mem; f.needsEncoder = enc; f.needsBenchmark = bench; f.requiresScenario = req; f.scenario = sc; f.fallbackId = fb; return f; };
  const uint64_t M = 1ull << 20;
  return {
    mk("basic_show", "Procedural scene + text/still + audio analysis", 0, 0, false, false, false, BenchId::BEN01, ""),   // BASE contract: always offered
    mk("audio_only_record", "Audio-only recording (compressor/limiter)", 5, 1 * M, false, true, true, BenchId::BEN01, ""),
    mk("duplicate_output", "Duplicate output (shared composition)", 5, 4 * M, false, true, true, BenchId::BEN02, ""),
    mk("independent_output", "Independent extra composition", 30, 12 * M, false, true, true, BenchId::BEN02, ""),
    mk("camera_treatment", "Camera input + treatments", 20, 6 * M, false, true, true, BenchId::BEN03, ""),
    mk("video_clip", "Video clip decode", 25, 8 * M, false, true, true, BenchId::BEN03, ""),
    mk("trails", "Trails / particles", 15, 4 * M, false, true, true, BenchId::BEN01, ""),
    mk("record_av", "Audio+video recording", 35, 10 * M, true, true, true, BenchId::BEN04, "audio_only_record"),
    mk("broadcast", "Broadcast encoder/transport", 35, 10 * M, true, true, true, BenchId::BEN05, "audio_only_record"),
  };
}

std::vector<std::string> CapabilityStatus::admittedFeatures() const {
  std::vector<std::string> v; for (auto& d : decisions) if (d.admission == Admission::Admitted || d.admission == Admission::Overridden) v.push_back(d.id); return v;
}
const FeatureDecision* CapabilityStatus::find(const std::string& id) const { for (auto& d : decisions) if (d.id == id) return &d; return nullptr; }

std::string CapabilityStatus::toJson() const {
  using json = nlohmann::json; json j;
  j["profile_hash"] = profileHash.empty() ? json(nullptr) : json(profileHash); j["profile_matched"] = profileMatched; j["tier"] = tier;
  j["measured"] = {{"cpu_headroom_pct", cpuHeadroomAvailable ? json(cpuHeadroomPct) : json("unavailable")},
                   {"frame_time_headroom_pct", frameHeadroomAvailable ? json(frameHeadroomPct) : json("unavailable")}, {"working_set_peak_bytes", wsPeakBytes}};
  j["unavailable_counters"] = unavailableCounters;
  json d = json::array();
  for (auto& x : decisions) d.push_back({{"id", x.id}, {"label", x.label}, {"admission", admissionName(x.admission)}, {"reason", x.reason}, {"overridable", x.overridable}, {"fallback", x.fallbackId}});
  j["features"] = d; j["admitted"] = admittedFeatures();
  j["adaptation"] = {{"level", adaptationLevel}, {"step", adaptationStep}, {"quality_locked", qualityLocked}, {"overload_active", overloadActive}};
  return j.dump(2);
}

CapabilityStatus AdmissionController::evaluate(const std::vector<FeatureDef>& req, const AdmissionContext& c) const {
  CapabilityStatus st; st.unavailableCounters = c.unavailableCounters; st.qualityLocked = c.qualityLocked; st.overloadActive = c.overloadActive;
  st.adaptationLevel = c.quality.level; st.adaptationStep = adaptStepName(c.quality.lastStep);
  // Profile validity (shared by all features)
  std::string profileProblem;
  const BenchProfile* p = c.profile;
  if (!p) profileProblem = "untested: no benchmark profile for this machine - run benchmark";
  else if (p->hardwareKey != c.hardwareKey) profileProblem = "untested: benchmark was run on different hardware/OS/display/power configuration - run benchmark";
  else if (p->appKey != c.appKey) profileProblem = "untested: application or dependencies changed since the benchmark - run benchmark";
  else if (c.nowWallMs < p->wallMs || c.nowWallMs - p->wallMs > c.maxProfileAgeMs) profileProblem = "untested: benchmark profile is too old - run benchmark";
  else if (!p->qualifying) profileProblem = "untested: benchmark profile is not qualifying (shortened, incomplete or isolation-violating run) - run benchmark";
  if (p && profileProblem.empty()) {
    st.profileMatched = true; st.profileHash = p->profileHash; st.tier = p->tier; st.cpuHeadroomAvailable = p->cpuHeadroomAvailable; st.cpuHeadroomPct = p->cpuHeadroomPct;
    st.frameHeadroomAvailable = p->frameHeadroomAvailable; st.frameHeadroomPct = p->frameHeadroomPct; st.wsPeakBytes = p->wsPeakBytes;
  } else if (p) st.profileHash = p->profileHash;
  double cpuAcc = c.cpuUsedPct; uint64_t memAcc = c.memUsedBytes;
  for (const auto& f : req) {
    FeatureDecision d; d.id = f.id; d.label = f.label; d.fallbackId = f.fallbackId;
    auto deny = [&](const std::string& why, bool overridable) { d.admission = Admission::Denied; d.reason = why; d.overridable = overridable; };
    auto untested = [&](const std::string& why) { d.admission = Admission::Untested; d.reason = why; d.overridable = true; };
    if (f.needsEncoder && !c.encoderAvailable) deny("encoder unavailable on this machine", false);
    else if (c.missingDeviceModes.count(f.id)) deny("required device mode is not available", false);
    else if (c.overloadActive && f.cpuCostPct > 0) deny(c.qualityLocked ? "system overloaded and quality is locked: added work rejected" : "system under pressure: added work rejected until load recovers", false);
    else if (memAcc + f.memBytes > c.memBudgetBytes) deny("exceeds memory budget (" + std::to_string((c.memBudgetBytes - std::min(memAcc, c.memBudgetBytes)) >> 20) + " MiB left)", false);
    else if (!f.needsBenchmark) { d.admission = Admission::Admitted; d.reason = "within BASE contract"; }
    else if (!profileProblem.empty()) untested(profileProblem);
    else if (f.requiresScenario && !p->passed(f.scenario)) deny(std::string("benchmark scenario ") + benchIdName(f.scenario) + " did not pass or was not run on the matching profile", true);
    else if (!p->cpuHeadroomAvailable) untested("headroom unknown: CPU counters were unavailable during the benchmark");
    else {
      double room = std::min(p->cpuHeadroomPct, c.cpuBudgetPct - cpuAcc);
      if (f.cpuCostPct > room) deny("insufficient measured headroom (estimated +" + std::to_string((int)f.cpuCostPct) + "% CPU, " + std::to_string((int)std::max(0.0, room)) + "% available)", true);
      else { d.admission = Admission::Admitted; d.reason = "admitted from matching benchmark profile (no guarantee of future performance)"; }
    }
    if ((d.admission == Admission::Untested || (d.admission == Admission::Denied && d.overridable)) && c.overrides.count(f.id)) {
      d.admission = Admission::Overridden; d.reason = "operator override: performance not guaranteed (was: " + d.reason + ")"; d.overridable = false;
    }
    if (d.admission == Admission::Admitted || d.admission == Admission::Overridden) { cpuAcc += f.cpuCostPct; memAcc += f.memBytes; }
    st.decisions.push_back(d);
  }
  return st;
}

} // namespace ss
