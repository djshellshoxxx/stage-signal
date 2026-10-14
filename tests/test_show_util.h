// Shared helpers for show/persistence tests (not auto-built: no test_ prefix .cpp).
#pragma once
#include "core/show/engine.h"
#include "core/show/model.h"
#include "test.h"
#include <string>
#include <vector>

#include <filesystem>
#include <cstdlib>
#include <unistd.h>
namespace sst {
using namespace ss;
using namespace ss::show;

inline ShowDocument baseDoc(int nScenes = 4) {
  static const char* kinds[] = {"spectrum_bars", "plasma", "tunnel", "color_field", "radial_pulse", "grid_pulse", "particle_field", "ripple_rings"};
  ShowDocument d;
  d.uuid = "123e4567-e89b-12d3-a456-426614174000";
  d.name = "Test Show";
  for (int i = 0; i < nScenes; ++i) { Scene s; s.id = "s" + std::to_string(i); s.name = "Scene " + std::to_string(i); s.rendererType = kinds[i % 8]; d.scenes.push_back(s); }
  Output a; a.logicalId = "main"; Output b; b.logicalId = "alt"; Output c; c.logicalId = "ctl"; c.role = OutputRole::Controls;
  d.outputs = {a, b, c};
  d.autoCfg.minDwellMs = 1000; d.autoCfg.maxDwellMs = 3000; d.autoCfg.quantize = Quantize::Immediate; d.autoCfg.repeatHistory = 2;
  d.autoCfg.transition = {"fade", 200};
  return d;
}
inline void makeStrobe(ShowDocument& d, int i) {
  d.scenes[(size_t)i].rendererType = "starburst"; d.scenes[(size_t)i].params["flash_rate"] = 6.0f; d.scenes[(size_t)i].strobe = true;
}
inline Cue msCue(const std::string& id, const std::string& scene, int64_t ms, Quantize q = Quantize::Immediate) {
  Cue c; c.id = id; c.sceneId = scene; c.duration.ms = ms; c.quantize = q; c.transition = {"cut", 0}; return c;
}
inline Cue beatCue(const std::string& id, const std::string& scene, double beats) {
  Cue c; c.id = id; c.sceneId = scene; c.duration.beats = true; c.duration.beatCount = beats; c.transition = {"cut", 0}; return c;
}
inline Action act(ActionId id, double v = 0, ActionSource src = ActionSource::UI) { Action a; a.id = id; a.value = v; a.source = src; return a; }

struct Rig {
  ShowEngine e; FakeClock clk; FeatureSnapshot f; uint64_t seq = 0;
  std::vector<std::pair<std::string, std::string>> journal;
  explicit Rig(const ShowDocument& d, EngineOptions o = EngineOptions()) : e(o) {
    e.setJournalHook([this](const std::string& k, const std::string& j) { journal.push_back({k, j}); });
    Status s = e.replaceDocument(d);
    if (!s.ok()) std::printf("  rig: replaceDocument failed: %s\n", s.detail.c_str());
    f.silent = false; f.tempo.valid = false;
    tick(10);
  }
  void tick(int ms, bool audioAlive = true) {
    for (int t = 0; t < ms; t += 10) { clk.advance(10 * kNsPerMs); if (audioAlive) f.sequence = ++seq; e.update(clk.t, f); }
  }
  ActionResult go(const Action& a) { return e.submit(a, clk.t); }
  std::vector<std::string> scenes() const {   // scene_change targets in order
    std::vector<std::string> v;
    for (const auto& j : journal) if (j.first == "scene_change") { size_t p = j.second.find("\"scene\":\""); if (p != std::string::npos) { p += 9; v.push_back(j.second.substr(p, j.second.find('"', p) - p)); } }
    return v;
  }
  int count(const std::string& kind) const { int n = 0; for (const auto& j : journal) if (j.first == kind) ++n; return n; }
};

struct TmpDir {
  std::filesystem::path p;
  TmpDir() {
    static int n = 0;
    p = std::filesystem::temp_directory_path() / ("ss_persist_" + std::to_string((long)getpid()) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p); std::filesystem::create_directories(p);
  }
  ~TmpDir() { std::error_code ec; std::filesystem::remove_all(p, ec); }
  std::string path(const std::string& rel = "") const { return rel.empty() ? p.string() : (p / rel).string(); }
};
} // namespace sst
