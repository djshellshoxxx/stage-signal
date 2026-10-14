// MOD-14 capability admission (spec §7 last paragraphs, IF-09 CapabilityStatus). Decides which features may be offered, from a
// MATCHING RECENT benchmark profile + encoder/device availability + memory/CPU budgets + measured headroom. It cannot guarantee
// future performance, never starts anything, and has no authority to enable transmission. Unqualified combinations are reported
// with reasons: "untested, run benchmark" (overridable) or denied (hard limits cannot be overridden).
// Documented override: the operator may acknowledge an Untested / measured-insufficient decision; the feature is then
// Overridden (still budgeted, labelled "performance not guaranteed", journal-worthy). Missing encoder, missing device mode,
// memory-cap overflow and active overload are never overridable.
#pragma once
#include "perf/benchmark.h"
#include "perf/adaptation.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ss {

struct FeatureDef {
  std::string id, label;
  double cpuCostPct = 0;               // estimated added average CPU (normalized %). PROVISIONAL until per-feature BEN data exists
  uint64_t memBytes = 0;               // estimated added working set
  bool needsEncoder = false;
  bool needsBenchmark = true;
  bool requiresScenario = false; BenchId scenario = BenchId::BEN01;   // scenario that must have passed in the profile
  std::string fallbackId;              // lighter alternative offered when unavailable (e.g. audio_only_record)
};
// Provisional catalog following spec §7 (basic profile always admissible; heavier features need measured admission).
std::vector<FeatureDef> defaultFeatureCatalog();

enum class Admission : uint8_t { Admitted, Untested, Denied, Overridden };
const char* admissionName(Admission a);
struct FeatureDecision { std::string id, label; Admission admission = Admission::Denied; std::string reason; bool overridable = false; std::string fallbackId; };

struct AdmissionContext {
  const BenchProfile* profile = nullptr;           // from BenchHistory::findMatching, or null
  std::string hardwareKey, appKey; int64_t nowWallMs = 0; int64_t maxProfileAgeMs = 30LL * 86400000;
  bool encoderAvailable = false; std::set<std::string> missingDeviceModes;   // feature ids whose device mode is unavailable
  uint64_t memBudgetBytes = 96ull << 20, memUsedBytes = 0; double cpuBudgetPct = 65, cpuUsedPct = 0;
  bool overloadActive = false, qualityLocked = false; QualityState quality;   // from AdaptationController
  std::set<std::string> overrides;                 // operator-acknowledged overrides (feature ids)
  std::vector<std::string> unavailableCounters;    // e.g. "cpu.busiest_core", "gpu.time": reported, never hidden
};
struct CapabilityStatus {                           // IF-09
  std::string profileHash; bool profileMatched = false; std::string tier = "untested";
  bool cpuHeadroomAvailable = false; double cpuHeadroomPct = 0; bool frameHeadroomAvailable = false; double frameHeadroomPct = 0; uint64_t wsPeakBytes = 0;
  std::vector<std::string> unavailableCounters;
  std::vector<FeatureDecision> decisions;           // in request order
  int adaptationLevel = 0; std::string adaptationStep = "none"; bool qualityLocked = false; bool overloadActive = false;
  std::vector<std::string> admittedFeatures() const;   // Admitted + Overridden
  const FeatureDecision* find(const std::string& id) const;
  std::string toJson() const;
};

class AdmissionController {
 public:
  CapabilityStatus evaluate(const std::vector<FeatureDef>& requested, const AdmissionContext& ctx) const;
};

} // namespace ss
