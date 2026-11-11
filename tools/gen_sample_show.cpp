// Authors samples/demo.stagesignal with the real show model + ShowIO (validated, atomic save).
// Build (host): see build_windows.sh comment; run from the repo root: gen_sample_show samples/demo.stagesignal
#include "core/show/model.h"
#include "persistence/show_io.h"
#include <cstdio>
using namespace ss; using namespace ss::show;

int main(int argc, char** argv) {
  const std::string out = argc > 1 ? argv[1] : "samples/demo.stagesignal";
  ShowDocument d; d.uuid = "5b6f2c10-7a3e-4d1c-9e55-0d3a8c4e2f01"; d.name = "StageSignal demo show";
  const char* names[12] = {"Spectrum Bars", "Waveform Ribbon", "Orbit", "Radial Pulse", "Particle Field", "Grid Pulse", "Tunnel", "Color Field", "Plasma", "Starburst", "Lissajous", "Ripple Rings"};
  const auto& pack = rendererPack();
  for (int i = 0; i < 12; ++i) { Scene s; s.id = "scene" + std::to_string(i + 1); s.name = names[i]; s.rendererType = pack[(size_t)i].id; s.cost = 2 + i % 4; d.scenes.push_back(s); }
  d.scenes[0].params["bars"] = 48; d.scenes[4].params["count"] = 600; d.scenes[8].params["scale"] = 3; d.scenes[11].params["rings"] = 10;
  Output a; a.logicalId = "main"; a.role = OutputRole::Audience; a.overlay.text = true; a.overlay.tempo = false; a.overlay.key = false;
  Output p; p.logicalId = "program"; p.role = OutputRole::Program; p.overlay.text = true;
  Output c; c.logicalId = "controls"; c.role = OutputRole::Controls; c.overlay.text = false;
  d.outputs = {a, p, c};
  // bundled CC0 test card copied next to the show (samples/media/), referenced by the first cue
  MediaRecord m; m.hash = "c617cc23fbad3750ff974ded1ce74bce5c9168b2c95c457411b426a5ea898b97"; m.relPath = "media/testcard_320x240.png"; m.format = "png"; m.bytes = 3840; d.media.push_back(m);
  d.texts = {{"welcome", "Welcome to StageSignal"}, {"thanks", "Thank you! See you next time"}, {"unicode", "Musik \xE2\x99\xAA  \xE2\x80\x94  \xD0\x9C\xD1\x83\xD0\xB7\xD1\x8B\xD0\xBA\xD0\xB0  \xE2\x80\x94  \xE9\x9F\xB3\xE6\xA5\xBD"}, {"break", "Back in 10 minutes"}};
  struct CueDef { const char* scene; int ms; const char* tr; int trMs; const char* text; };
  CueDef cues[] = {{"scene8", 10000, "fade", 800, "welcome"}, {"scene1", 12000, "fade", 600, ""}, {"scene3", 12000, "dip_black", 700, ""}, {"scene7", 10000, "wipe", 500, ""},
                   {"scene9", 12000, "fade", 900, ""}, {"scene12", 0, "cut", 0, "break"}, {"scene5", 12000, "fade", 600, ""}, {"scene11", 15000, "fade", 1000, "thanks"}};
  int n = 1; for (auto& cd : cues) { Cue q; q.id = "cue" + std::to_string(n++); q.sceneId = cd.scene; q.duration.ms = cd.ms; q.transition.type = cd.tr; q.transition.ms = cd.trMs; q.textRef = cd.text; if (n == 2) q.mediaRef = m.hash; d.cues.push_back(q); }
  d.cueEnd = CueListEnd::Loop; d.defaultMode = 0;
  d.autoCfg.seed = 42; d.autoCfg.minDwellMs = 8000; d.autoCfg.maxDwellMs = 16000; d.autoCfg.repeatHistory = 3;
  d.idle.screen = "black"; d.idle.audioLoss = AudioLossPolicy::Continue;
  d.patch = {R"({"id":"par1","profile":"generic-rgb","label":"Front left","universe":1,"address":1})", R"({"id":"par2","profile":"generic-rgb","label":"Front right","universe":1,"address":4})"};
  Status v = validateShow(d); if (!v.ok()) { std::fprintf(stderr, "invalid sample: %s\n", v.detail.c_str()); return 2; }
  persist::ShowIO io; Status s = io.save(out, d); if (!s.ok()) { std::fprintf(stderr, "save failed: %s\n", s.detail.c_str()); return 1; }
  std::printf("wrote %s (%zu scenes, %zu cues, %zu messages)\n", out.c_str(), d.scenes.size(), d.cues.size(), d.texts.size()); return 0;
}
