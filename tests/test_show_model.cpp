#include "test_show_util.h"
using namespace sst;

static bool rejects(const ShowDocument& d, Err code = Err::InvalidArgument) { Status s = validateShow(d); return !s.ok() && s.code == code; }

TEST(model_valid_base_and_pack) {
  CHECK(validateShow(baseDoc(4)).ok());
  CHECK(rendererPack().size() == 12);
  static const char* ids[] = {"spectrum_bars", "waveform_ribbon", "geometric_orbit", "radial_pulse", "particle_field", "grid_pulse", "tunnel", "color_field", "plasma", "starburst", "scope_lissajous", "ripple_rings"};
  for (auto* id : ids) CHECK(findRenderer(id) != nullptr);
  CHECK(findRenderer("projectm") == nullptr);
  for (const auto& r : rendererPack()) for (const auto& p : r.params) CHECK(p.min <= p.def && p.def <= p.max);
}

TEST(model_import_limits) {
  ShowDocument d = baseDoc(1);
  d.scenes.clear();
  for (int i = 0; i < 1000; ++i) { Scene s; s.id = "s" + std::to_string(i); s.rendererType = "plasma"; d.scenes.push_back(s); }
  CHECK(validateShow(d).ok());
  Scene extra; extra.id = "sx"; extra.rendererType = "plasma"; d.scenes.push_back(extra);
  CHECK(rejects(d));
  d.scenes.pop_back();
  for (int i = 0; i < 10000; ++i) d.cues.push_back(msCue("c" + std::to_string(i), "s0", 100));
  CHECK(validateShow(d).ok());
  d.cues.push_back(msCue("cx", "s0", 100)); CHECK(rejects(d)); d.cues.pop_back(); d.cues.clear();
  for (int i = 0; i < 256; ++i) d.texts.push_back({"t" + std::to_string(i), "hi"});
  CHECK(validateShow(d).ok()); d.texts.push_back({"tx", "hi"}); CHECK(rejects(d)); d.texts.clear();
  for (int i = 0; i < 512; ++i) d.midiMappings.push_back("{\"n\":" + std::to_string(i) + "}");
  CHECK(validateShow(d).ok()); d.midiMappings.push_back("{}"); CHECK(rejects(d)); d.midiMappings.clear();
  for (int i = 0; i < 256; ++i) { d.fixtureProfiles.push_back("{}"); d.patch.push_back("{}"); }
  CHECK(validateShow(d).ok()); d.patch.push_back("{}"); CHECK(rejects(d)); d.patch.pop_back();
  d.fixtureProfiles.push_back("{}"); CHECK(rejects(d)); d.fixtureProfiles.clear(); d.patch.clear();
  // text code points
  std::string t; for (int i = 0; i < 4096; ++i) t += "\xF0\x9F\x8E\xB5";
  d.texts = {{"a", t}}; CHECK(validateShow(d).ok());
  d.texts = {{"a", t + "x"}}; CHECK(rejects(d));
  d.texts = {{"a", "\xED\xA0\x80"}}; CHECK(rejects(d));               // surrogate
  d.texts = {{"a", "\xC0\x80"}}; CHECK(rejects(d));                   // overlong
  d.texts.clear();
  for (int i = 0; i < 17; ++i) { Output o; o.logicalId = "o" + std::to_string(i); d.outputs.push_back(o); }
  CHECK(rejects(d)); d.outputs.clear();
  std::string big(70000, 'x'); d.midiMappings = {"{\"a\":\"" + big + "\"}"}; CHECK(rejects(d));
}

TEST(model_scene_param_ranges_and_strobe_flag) {
  ShowDocument d = baseDoc(2);
  d.scenes[0].params["bars"] = 64; CHECK(validateShow(d).ok());
  d.scenes[0].params["bars"] = 1000; CHECK(rejects(d));
  d.scenes[0].params["bars"] = 64; d.scenes[0].params["bogus"] = 1; CHECK(rejects(d));
  d.scenes[0].params.erase("bogus");
  d.scenes[0].params["gain"] = NAN; CHECK(rejects(d)); d.scenes[0].params.erase("gain");
  d.scenes[1].rendererType = "starburst"; d.scenes[1].params["flash_rate"] = 3;
  CHECK(rejects(d));                                                  // flashing scene must carry strobe flag
  d.scenes[1].strobe = true; CHECK(validateShow(d).ok());
  d.scenes[1].cost = 0; CHECK(rejects(d)); d.scenes[1].cost = 11; CHECK(rejects(d)); d.scenes[1].cost = 5; CHECK(validateShow(d).ok());
  auto rp = resolvedParams(d.scenes[0]); CHECK(rp["bars"] == 64 && rp["gain"] == 1.0f);
  d.scenes[0].rendererType = "nope"; CHECK(rejects(d));
}

TEST(model_references_and_security) {
  ShowDocument d = baseDoc(2);
  d.cues = {msCue("c0", "s9", 100)}; CHECK(rejects(d));
  d.cues = {msCue("c0", "s0", 100), msCue("c0", "s1", 100)}; CHECK(rejects(d));       // duplicate id
  d.cues = {msCue("c0", "", 100)}; CHECK(validateShow(d).ok());                      // empty scene = keep current
  d.cues[0].textRef = "none"; CHECK(rejects(d)); d.cues[0].textRef.clear();
  d.cues[0].mediaRef = std::string(64, 'a'); CHECK(rejects(d));
  d.cues[0].mediaRef.clear();
  d.cues[0].duration.ms = -1; CHECK(rejects(d)); d.cues[0].duration.ms = 100;
  d.cues[0].duration.beats = true; d.cues[0].duration.beatCount = 5000; CHECK(rejects(d));
  d.cues[0].duration.beatCount = 8; CHECK(validateShow(d).ok());
  d.cues[0].transition = {"spin", 10}; CHECK(rejects(d)); d.cues[0].transition = {"fade", 70000}; CHECK(rejects(d));
  d.cues.clear();
  MediaRecord m; m.hash = std::string(64, 'a'); m.relPath = "media/a.png"; m.format = "png"; d.media = {m};
  CHECK(validateShow(d).ok());
  const char* badPaths[] = {"../a.png", "/abs/a.png", "C:\\a.png", "\\\\srv\\x\\a.png", "a/../../b.png", "http://x/a.png", "a//b.png", "a/./b.png", "a.png.", "con.png"};
  for (const char* bp : badPaths) { d.media[0].relPath = bp; CHECK(!validateShow(d).ok()); }
  d.media[0].relPath = "media/a.png"; d.media[0].format = "exe"; CHECK(rejects(d)); d.media[0].format = "svg"; CHECK(rejects(d));
  d.media[0].format = "mp4"; CHECK(validateShow(d).ok());
  d.media[0].hash = "xyz"; CHECK(rejects(d)); d.media[0].hash = std::string(64, 'a');
  d.midiMappings = {"{\"script\":\"rm -rf\"}"}; CHECK(rejects(d));                  // executable behaviour
  d.midiMappings = {"{\"a\":{\"b\":{\"URL\":\"http://x\"}}}"}; CHECK(rejects(d));
  d.midiMappings = {"[1,2]"}; CHECK(rejects(d)); d.midiMappings = {"not json"}; CHECK(rejects(d));
  d.midiMappings = {"{\"note\":60,\"action\":\"NextScene\"}"}; CHECK(validateShow(d).ok());
  d.uuid = "not-a-uuid"; CHECK(rejects(d)); d.uuid = "123e4567-e89b-12d3-a456-426614174000";
  d.autoCfg.pool = {{"s0", 1}, {"s0", 2}}; CHECK(rejects(d)); d.autoCfg.pool = {{"s7", 1}}; CHECK(rejects(d)); d.autoCfg.pool = {{"s0", 1001}}; CHECK(rejects(d)); d.autoCfg.pool.clear();
  d.autoCfg.minDwellMs = 100; CHECK(rejects(d)); d.autoCfg.minDwellMs = 5000; d.autoCfg.maxDwellMs = 4000; CHECK(rejects(d));
  d.autoCfg.maxDwellMs = 6000; d.idle.audioLossMs = 5000; CHECK(rejects(d)); d.idle.audioLossMs = 1000;   // keeps REQ-D09 within 2 s
  d.meta["bad"] = "1"; CHECK(rejects(d)); d.meta.clear(); d.meta["x_note"] = "\"hi\""; CHECK(validateShow(d).ok());
  d.schemaMajor = 2; CHECK(rejects(d, Err::SchemaUnsupported));
}

TEST(model_output_overlay_policy) {
  ShowDocument d = baseDoc(1);
  d.outputs[0].overlay.tempo = true; d.outputs[0].overlay.key = false; d.outputs[0].overlay.branding = true; d.outputs[0].overlay.brandingRef = std::string(64, 'b');
  CHECK(rejects(d));                                                  // branding must reference included media
  MediaRecord m; m.hash = std::string(64, 'b'); m.relPath = "media/b.png"; m.format = "png"; d.media = {m};
  CHECK(validateShow(d).ok());
  d.outputs[1].logicalId = "main"; CHECK(rejects(d));
}

int main() { return ss_test::runAll(); }
