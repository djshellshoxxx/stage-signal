// MOD-06 scene pack: exactly 12 original procedural scenes (spec §13). Pure registry metadata; rendering lives in Renderer.
#pragma once
#include "render/render_state.h"

namespace ss {

enum class CostClass : uint8_t { Low, Medium, High };           // relative per-frame cost at equal internal size
struct ParamDef { const char* key; const char* label; float min, max, def; };

constexpr int kSceneCount = 12;
constexpr int kMaxSceneParams = 8;                              // params[0..3] = brightness,speed,intensity,hue; [4..] = extra[0..]
struct SceneInfo {
  const char* id;            // stable id e.g. "spectrum_bars"
  const char* name;          // display name
  CostClass cost;
  bool strobe;               // may produce rapid luminance flashes; disabled entirely by RenderState.disableStrobing
  int fallback;              // cheaper scene index used under shed (self if already cheapest)
  int nParams;
  ParamDef params[kMaxSceneParams];
};

int sceneCount();                                               // == 12
const SceneInfo& sceneInfo(int index);                          // index clamped into range
int sceneIndexById(const char* id);                             // -1 if unknown
SceneParams defaultParams(int index);                           // registry defaults (extras filled in)
SceneParams clampParams(int index, const SceneParams& p);       // clamps to ranges, resolves kParamDefault

} // namespace ss
