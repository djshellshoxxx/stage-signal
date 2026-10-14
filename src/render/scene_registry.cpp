// Generated-style table (hand-maintained): scene metadata. See scene_registry.h.
#include "render/scene_registry.h"
#include <cstring>

namespace ss {
namespace {
const SceneInfo kScenes[kSceneCount] = {
  {"spectrum_bars", "Spectrum Bars", CostClass::Low, false, 0, 8, {{"brightness","Brightness",0.0f,1.0f,1.0f}, {"speed","Speed",0.25f,4.0f,1.0f}, {"intensity","Intensity",0.0f,1.0f,0.5f}, {"hue","Hue",0.0f,1.0f,0.0f}, {"bars","Bars",8.0f,64.0f,24.0f}, {"gap","Gap",0.0f,0.8f,0.2f}, {"mirror","Mirror",0.0f,1.0f,0.0f}, {"caps","Peak caps",0.0f,1.0f,1.0f}}},
  {"waveform_ribbon", "Waveform Ribbon", CostClass::Low, false, 1, 8, {{"brightness","Brightness",0.0f,1.0f,1.0f}, {"speed","Speed",0.25f,4.0f,1.0f}, {"intensity","Intensity",0.0f,1.0f,0.5f}, {"hue","Hue",0.0f,1.0f,0.0f}, {"thickness","Thickness",0.02f,0.3f,0.08f}, {"ribbons","Ribbons",1.0f,3.0f,2.0f}, {"frequency","Frequency",0.5f,4.0f,1.5f}, {"amplitude","Amplitude",0.3f,1.2f,1.0f}}},
  {"geometric_orbit", "Geometric Orbit", CostClass::Low, false, 2, 8, {{"brightness","Brightness",0.0f,1.0f,1.0f}, {"speed","Speed",0.25f,4.0f,1.0f}, {"intensity","Intensity",0.0f,1.0f,0.5f}, {"hue","Hue",0.0f,1.0f,0.0f}, {"bodies","Bodies",3.0f,16.0f,7.0f}, {"connect","Connect",0.0f,1.0f,1.0f}, {"size","Body size",0.02f,0.15f,0.06f}, {"rings","Rings",1.0f,3.0f,2.0f}}},
  {"radial_pulse", "Radial Pulse", CostClass::Medium, false, 7, 8, {{"brightness","Brightness",0.0f,1.0f,1.0f}, {"speed","Speed",0.25f,4.0f,1.0f}, {"intensity","Intensity",0.0f,1.0f,0.5f}, {"hue","Hue",0.0f,1.0f,0.0f}, {"density","Ring density",1.0f,16.0f,6.0f}, {"pulse","Pulse depth",0.0f,1.0f,0.5f}, {"falloff","Falloff",0.0f,1.0f,0.5f}, {"swirl","Swirl",0.0f,1.0f,0.0f}}},
  {"particle_field", "Particle Field", CostClass::Medium, false, 2, 8, {{"brightness","Brightness",0.0f,1.0f,1.0f}, {"speed","Speed",0.25f,4.0f,1.0f}, {"intensity","Intensity",0.0f,1.0f,0.5f}, {"hue","Hue",0.0f,1.0f,0.0f}, {"count","Particles",50.0f,1024.0f,300.0f}, {"size","Size",0.5f,4.0f,1.5f}, {"flow","Flow rate",0.3f,3.0f,1.0f}, {"bloom","Onset bloom",0.0f,1.0f,0.5f}}},
  {"grid_pulse", "Grid Pulse", CostClass::Low, true, 5, 8, {{"brightness","Brightness",0.0f,1.0f,1.0f}, {"speed","Speed",0.25f,4.0f,1.0f}, {"intensity","Intensity",0.0f,1.0f,0.5f}, {"hue","Hue",0.0f,1.0f,0.0f}, {"cells","Columns",4.0f,32.0f,12.0f}, {"gap","Cell gap",0.0f,1.0f,0.5f}, {"wave","Wave frequency",0.5f,4.0f,1.5f}, {"flash","Beat flash",0.0f,1.0f,0.8f}}},
  {"tunnel", "Tunnel", CostClass::Medium, false, 7, 8, {{"brightness","Brightness",0.0f,1.0f,1.0f}, {"speed","Speed",0.25f,4.0f,1.0f}, {"intensity","Intensity",0.0f,1.0f,0.5f}, {"hue","Hue",0.0f,1.0f,0.0f}, {"segments","Segments",2.0f,16.0f,8.0f}, {"twist","Twist",0.0f,1.0f,0.4f}, {"stripes","Stripes",1.0f,8.0f,4.0f}, {"fog","Depth fog",0.0f,1.0f,0.5f}}},
  {"color_field", "Color Field", CostClass::Low, false, 7, 8, {{"brightness","Brightness",0.0f,1.0f,1.0f}, {"speed","Speed",0.25f,4.0f,1.0f}, {"intensity","Intensity",0.0f,1.0f,0.5f}, {"hue","Hue",0.0f,1.0f,0.0f}, {"drift","Hue drift",0.0f,1.0f,0.5f}, {"contrast","Saturation",0.0f,1.0f,0.5f}, {"swell","Bass swell",0.0f,1.0f,0.4f}, {"tilt","Tilt",0.0f,1.0f,0.3f}}},
  {"plasma", "Plasma", CostClass::High, false, 7, 8, {{"brightness","Brightness",0.0f,1.0f,1.0f}, {"speed","Speed",0.25f,4.0f,1.0f}, {"intensity","Intensity",0.0f,1.0f,0.5f}, {"hue","Hue",0.0f,1.0f,0.0f}, {"scale","Scale",0.5f,3.0f,1.2f}, {"warp","Diagonal warp",0.0f,1.0f,0.5f}, {"span","Color span",0.0f,1.0f,0.5f}, {"cycles","Palette cycles",0.5f,4.0f,1.5f}}},
  {"starburst", "Starburst", CostClass::Medium, true, 2, 8, {{"brightness","Brightness",0.0f,1.0f,1.0f}, {"speed","Speed",0.25f,4.0f,1.0f}, {"intensity","Intensity",0.0f,1.0f,0.5f}, {"hue","Hue",0.0f,1.0f,0.0f}, {"rays","Rays",8.0f,64.0f,24.0f}, {"length","Length",0.3f,1.0f,0.8f}, {"spin","Spin",0.0f,1.0f,0.4f}, {"flash","Onset flash",0.0f,1.0f,0.8f}}},
  {"scope_lissajous", "Scope Lissajous", CostClass::Low, false, 10, 8, {{"brightness","Brightness",0.0f,1.0f,1.0f}, {"speed","Speed",0.25f,4.0f,1.0f}, {"intensity","Intensity",0.0f,1.0f,0.5f}, {"hue","Hue",0.0f,1.0f,0.0f}, {"a","Ratio A",1.0f,7.0f,3.0f}, {"b","Ratio B",1.0f,7.0f,2.0f}, {"thickness","Thickness",1.0f,4.0f,1.5f}, {"points","Points",64.0f,1024.0f,512.0f}}},
  {"ripple_rings", "Ripple Rings", CostClass::Medium, false, 2, 8, {{"brightness","Brightness",0.0f,1.0f,1.0f}, {"speed","Speed",0.25f,4.0f,1.0f}, {"intensity","Intensity",0.0f,1.0f,0.5f}, {"hue","Hue",0.0f,1.0f,0.0f}, {"rings","Rings",2.0f,8.0f,5.0f}, {"thickness","Thickness",0.01f,0.08f,0.03f}, {"period","Period (s)",1.0f,6.0f,3.0f}, {"spread","Spread",0.0f,1.0f,0.6f}}},
};
} // namespace

int sceneCount() { return kSceneCount; }
const SceneInfo& sceneInfo(int i) { return kScenes[i < 0 ? 0 : (i >= kSceneCount ? kSceneCount - 1 : i)]; }
int sceneIndexById(const char* id) {
  if (!id) return -1;
  for (int i = 0; i < kSceneCount; ++i) if (!std::strcmp(kScenes[i].id, id)) return i;
  return -1;
}
SceneParams defaultParams(int index) {
  const SceneInfo& s = sceneInfo(index); SceneParams p;
  p.brightness = s.params[0].def; p.speed = s.params[1].def; p.intensity = s.params[2].def; p.hue = s.params[3].def;
  for (int i = 0; i < 4; ++i) p.extra[i] = s.params[4 + i].def;
  return p;
}
static float clampf(float v, float lo, float hi) { return v != v ? lo : (v < lo ? lo : (v > hi ? hi : v)); }
SceneParams clampParams(int index, const SceneParams& in) {
  const SceneInfo& s = sceneInfo(index); SceneParams p;
  p.brightness = clampf(in.brightness, s.params[0].min, s.params[0].max);
  p.speed = clampf(in.speed, s.params[1].min, s.params[1].max);
  p.intensity = clampf(in.intensity, s.params[2].min, s.params[2].max);
  p.hue = clampf(in.hue, s.params[3].min, s.params[3].max);
  for (int i = 0; i < 4; ++i) { const ParamDef& d = s.params[4 + i]; p.extra[i] = in.extra[i] <= kParamDefault * 0.5f ? d.def : clampf(in.extra[i], d.min, d.max); }
  return p;
}

} // namespace ss
