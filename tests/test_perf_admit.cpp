#include "test.h"
#include "perf/admission.h"
#include "nlohmann/json.hpp"
using namespace ss;
using json = nlohmann::json;

static BenchProfile goodProfile() {
  BenchProfile p; p.profileHash = "ph"; p.comparableKey = "ck"; p.hardwareKey = "hw"; p.appKey = "app"; p.tier = "BASE"; p.wallMs = 1000000000000LL; p.qualifying = true; p.gatesPassed = true;
  p.cpuHeadroomAvailable = true; p.cpuHeadroomPct = 30; p.frameHeadroomAvailable = true; p.frameHeadroomPct = 20; p.wsPeakBytes = 80ull << 20;
  p.scenariosPassed = {"BEN-01", "BEN-02", "BEN-03", "BEN-04", "BEN-05", "BEN-06"}; return p;
}
static AdmissionContext ctxFor(const BenchProfile* p) {
  AdmissionContext c; c.profile = p; c.hardwareKey = "hw"; c.appKey = "app"; c.nowWallMs = 1000000000000LL + 86400000LL; c.encoderAvailable = true; c.memUsedBytes = 60ull << 20; return c;
}
static const FeatureDef& feat(const std::vector<FeatureDef>& v, const char* id) { for (auto& f : v) if (f.id == id) return f; static FeatureDef none; return none; }

TEST(admit_no_profile_is_untested_run_benchmark) {
  auto cat = defaultFeatureCatalog(); AdmissionController ac; CapabilityStatus st = ac.evaluate(cat, ctxFor(nullptr));
  CHECK(st.find("basic_show")->admission == Admission::Admitted);                  // BASE contract needs no profile
  for (const char* id : {"audio_only_record", "record_av", "broadcast", "video_clip", "independent_output", "camera_treatment"}) {
    const FeatureDecision* d = st.find(id); CHECK(d && d->admission == Admission::Untested && d->overridable && d->reason.find("run benchmark") != std::string::npos);
  }
  CHECK(!st.profileMatched && st.tier == "untested");
}
TEST(admit_matching_profile_admits_within_headroom) {
  BenchProfile p = goodProfile(); auto cat = defaultFeatureCatalog(); AdmissionController ac; AdmissionContext c = ctxFor(&p); c.cpuUsedPct = 0;
  CapabilityStatus st = ac.evaluate({feat(cat, "basic_show"), feat(cat, "audio_only_record"), feat(cat, "duplicate_output")}, c);
  CHECK(st.profileMatched && st.profileHash == "ph" && st.tier == "BASE" && st.cpuHeadroomAvailable && st.cpuHeadroomPct == 30 && st.wsPeakBytes == (80ull << 20));
  CHECK(st.admittedFeatures().size() == 3);
  // budget accumulates: record_av (35%) does not fit in 30% headroom => denied with measured reason, overridable
  st = ac.evaluate({feat(cat, "record_av")}, c); const FeatureDecision* d = st.find("record_av");
  CHECK(d->admission == Admission::Denied && d->overridable && d->reason.find("headroom") != std::string::npos && d->fallbackId == "audio_only_record");
  CapabilityStatus two = ac.evaluate({feat(cat, "video_clip"), feat(cat, "camera_treatment")}, c);       // 25 then 20: second exceeds remaining 5
  CHECK(two.find("video_clip")->admission == Admission::Admitted && two.find("camera_treatment")->admission == Admission::Denied);
}
TEST(admit_profile_must_match_recent_and_qualifying) {
  auto cat = defaultFeatureCatalog(); AdmissionController ac; std::vector<FeatureDef> req = {feat(cat, "audio_only_record")};
  BenchProfile p = goodProfile(); AdmissionContext c = ctxFor(&p);
  c.hardwareKey = "other-hw"; CHECK(ac.evaluate(req, c).decisions[0].admission == Admission::Untested && ac.evaluate(req, c).decisions[0].reason.find("hardware") != std::string::npos);
  c = ctxFor(&p); c.appKey = "app2"; CHECK(ac.evaluate(req, c).decisions[0].reason.find("application") != std::string::npos);
  c = ctxFor(&p); c.nowWallMs += 40LL * 86400000; CHECK(ac.evaluate(req, c).decisions[0].reason.find("too old") != std::string::npos);
  c = ctxFor(&p); c.nowWallMs = p.wallMs - 1; CHECK(ac.evaluate(req, c).decisions[0].admission == Admission::Untested);       // clock went backwards
  BenchProfile scaled = goodProfile(); scaled.qualifying = false; c = ctxFor(&scaled); CHECK(ac.evaluate(req, c).decisions[0].reason.find("not qualifying") != std::string::npos);
  BenchProfile noHead = goodProfile(); noHead.cpuHeadroomAvailable = false; c = ctxFor(&noHead); CHECK(ac.evaluate(req, c).decisions[0].reason.find("unknown") != std::string::npos);
}
TEST(admit_scenario_must_have_passed) {
  BenchProfile p = goodProfile(); p.scenariosPassed = {"BEN-01", "BEN-06"}; auto cat = defaultFeatureCatalog(); AdmissionController ac; AdmissionContext c = ctxFor(&p);
  CapabilityStatus st = ac.evaluate({feat(cat, "record_av")}, c); CHECK(st.decisions[0].admission == Admission::Denied && st.decisions[0].reason.find("BEN-04") != std::string::npos);
}
TEST(admit_hard_limits_cannot_be_overridden) {
  BenchProfile p = goodProfile(); auto cat = defaultFeatureCatalog(); AdmissionController ac; AdmissionContext c = ctxFor(&p);
  c.encoderAvailable = false; c.overrides = {"record_av", "broadcast"};
  CapabilityStatus st = ac.evaluate({feat(cat, "record_av"), feat(cat, "broadcast")}, c);
  for (auto& d : st.decisions) CHECK(d.admission == Admission::Denied && !d.overridable && d.reason.find("encoder") != std::string::npos);
  c = ctxFor(&p); c.memUsedBytes = 90ull << 20; c.overrides = {"independent_output"}; st = ac.evaluate({feat(cat, "independent_output")}, c);
  CHECK(st.decisions[0].admission == Admission::Denied && st.decisions[0].reason.find("memory") != std::string::npos);
  c = ctxFor(&p); c.missingDeviceModes = {"camera_treatment"}; c.overrides = {"camera_treatment"}; CHECK(ac.evaluate({feat(cat, "camera_treatment")}, c).decisions[0].admission == Admission::Denied);
}
TEST(admit_documented_override_for_untested_and_insufficient_headroom) {
  auto cat = defaultFeatureCatalog(); AdmissionController ac; AdmissionContext c = ctxFor(nullptr); c.overrides = {"video_clip"};
  CapabilityStatus st = ac.evaluate({feat(cat, "video_clip"), feat(cat, "camera_treatment")}, c);
  CHECK(st.decisions[0].admission == Admission::Overridden && st.decisions[0].reason.find("performance not guaranteed") != std::string::npos && !st.decisions[0].overridable);
  CHECK(st.decisions[1].admission == Admission::Untested); CHECK(st.admittedFeatures() == std::vector<std::string>{"video_clip"});
  BenchProfile p = goodProfile(); c = ctxFor(&p); c.overrides = {"record_av"}; CHECK(ac.evaluate({feat(cat, "record_av")}, c).decisions[0].admission == Admission::Overridden);
}
TEST(admit_overload_and_locked_quality_reject_added_work) {
  BenchProfile p = goodProfile(); auto cat = defaultFeatureCatalog(); AdmissionController ac; AdmissionContext c = ctxFor(&p); c.overloadActive = true; c.qualityLocked = true; c.overrides = {"video_clip"};
  CapabilityStatus st = ac.evaluate({feat(cat, "basic_show"), feat(cat, "video_clip")}, c);
  CHECK(st.decisions[0].admission == Admission::Admitted); CHECK(st.decisions[1].admission == Admission::Denied && st.decisions[1].reason.find("locked") != std::string::npos);
  CHECK(st.qualityLocked && st.overloadActive);
}
TEST(admit_capability_status_json_if09) {
  BenchProfile p = goodProfile(); auto cat = defaultFeatureCatalog(); AdmissionController ac; AdmissionContext c = ctxFor(&p);
  c.quality.level = 3; c.quality.lastStep = AdaptStep::RenderScale; c.unavailableCounters = {"cpu.busiest_core", "gpu.time"};
  json j = json::parse(ac.evaluate(cat, c).toJson());
  CHECK(j["profile_hash"] == "ph" && j["measured"]["cpu_headroom_pct"] == 30 && j["adaptation"]["step"] == "render_scale" && j["adaptation"]["level"] == 3);
  CHECK(j["unavailable_counters"].size() == 2 && j["features"].size() == cat.size() && j["admitted"].size() >= 1);
  AdmissionContext n = ctxFor(nullptr); n.unavailableCounters = {"cpu"}; json k = json::parse(ac.evaluate(cat, n).toJson());
  CHECK(k["profile_hash"].is_null() && k["measured"]["cpu_headroom_pct"] == "unavailable");
}
TEST(admit_history_roundtrip_bounded_and_matching) {
  BenchHistory h; for (int i = 0; i < 20; ++i) { BenchProfile p = goodProfile(); p.profileHash = "p" + std::to_string(i); p.wallMs += i * 1000; p.hardwareKey = i < 10 ? "hwA" : "hw"; h.add(p); }
  CHECK(h.size() == 16);
  const BenchProfile* m = h.findMatching("hw", "app", 1000000000000LL + 30000, 86400000); CHECK(m && m->profileHash == "p19");              // newest match wins
  CHECK(h.findMatching("hw", "app", 1000000000000LL + 10 * 86400000LL, 86400000) == nullptr);
  CHECK(h.findMatching("nope", "app", 1000000000000LL, 86400000) == nullptr);
  BenchHistory g; CHECK(g.fromJson(h.toJson()) && g.size() == 16); CHECK(g.findMatching("hw", "app", 1000000000000LL + 30000, 86400000)->profileHash == "p19");
  CHECK(!g.fromJson("garbage") && !g.fromJson("[{\"x\":1}]") && g.size() == 16);
  BenchProfile out; CHECK(!BenchProfile::fromJson("{}", out) && BenchProfile::fromJson(goodProfile().toJson(), out) && out.passed(BenchId::BEN04));
}
int main() { return ss_test::runAll(); }
