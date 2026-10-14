// RenderState composition (spec §13, IF-05 -> IF-07): pure functions that turn a published ShowView (document + ShowSnapshot +
// effective features) into the renderer's RenderState for ONE logical output. No threads, no I/O: unit-testable.
#pragma once
#include "core/app/app_types.h"
#include "render/render_state.h"
#include "render/scene_registry.h"
#include <map>
#include <set>

namespace ss { namespace app {

// Immutable bundle published by the control thread after every update (the only way workers see show state).
struct ShowView {
  std::shared_ptr<const show::ShowDocument> doc;
  show::ShowSnapshot snap;
  FeatureSnapshot features;          // effective features (manual/tap tempo and key merged by the show engine)
  uint64_t docGeneration = 0;        // bumps when the document is replaced
  Ns publishedNs = 0;
};
using ShowViewPtr = std::shared_ptr<const ShowView>;

// Scene-local animation clocks for one output: the time a scene became visible (elapsed-time driven, frame-skip safe).
class SceneClocks {
 public:
  Ns elapsed(int docScene, Ns now);                 // starts the clock on first use
  void keepOnly(const std::set<int>& visible);      // forget scenes no longer on screen
  void reset() { start_.clear(); }
 private:
  std::map<int, Ns> start_;
};

struct ComposeExtras {
  Ns elapsedNs = 0;                  // monotonic show time
  uint32_t generation = 0;
  bool refBorder = false;            // audience diagnostics (explicitly enabled only)
  const Frame* media = nullptr;      // cue media frame (placeholder when failed)
  const Frame* branding = nullptr;   // approved branding image
  const Frame* camera = nullptr;     // newest camera frame (null when hidden/frozen too long)
  bool cameraRouted = false;         // deliberate publication to THIS output
  CameraTreatments cameraTreatments;
  float internalScale = 0.0f;        // adaptation render scale (0 = configured)
  int shedLevel = 0;                 // ShedKnobs::forLevel input
  bool forceBlackout = false;
  int aspectNum = 0, aspectDen = 0;
  bool allowOverlays = true;         // operator preview: overlays off
};

std::u32string utf8ToU32(const std::string& s);                 // invalid sequences -> U+FFFD
int registryIndexForScene(const show::ShowDocument& d, int docScene);   // -1 unknown/none
// outputIndex: index in snap.outputs (and doc.outputs); -1 = program-level composition (operator preview default).
RenderState composeState(const ShowView& v, int outputIndex, SceneClocks& clocks, const ComposeExtras& x);
// Exact byte key of everything that influences pixels (duplicate routes with equal keys share ONE rendered composition).
std::string compositionKey(const RenderState& s, int outW, int outH);
// Nearest-neighbour aspect-fit (letterbox) scale for encoders whose profile size differs from the composition.
void fitScaleFrame(const Frame& in, int w, int h, Frame& out);

}} // namespace ss::app
