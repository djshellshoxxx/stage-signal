// MOD-06 software renderer (spec §13, §7 minimum-machine contract). Scalar C++17, integer pixel math, no SIMD/GL.
// Thread model: one render thread calls configure()/render(); Frames referenced by RenderState are read-only for the call.
// Memory: pools sized once in configure(); render() does not allocate except text-cache misses (use prepareText() ahead of
// publication) and resizing `out` if its dimensions differ from the configured output.
#pragma once
#include "core/ports.h"
#include "render/render_state.h"
#include "render/scene_registry.h"
#include <memory>

namespace ss {

struct IRect { int x = 0, y = 0, w = 0, h = 0; };

struct RenderStats {
  int internalW = 0, internalH = 0;     // composition buffer size actually used for the last frame
  IRect fit;                            // aspect-fit rect inside the output (REQ-D04/D07)
  bool halfResTransition = false;
  bool strobeGuardActive = false;
  bool trailsActive = false;
  int sceneA = -1, sceneB = -1;         // effective scene indices after shed fallback (-1 not rendered)
};

class Renderer {
public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  // Allocates all pools for an output of outW x outH (16..4096 each). renderScale (0.1..1) is the MAXIMUM internal scale
  // (RenderState::internalScale may only lower it). Re-callable on resolution/rotation change (content recomputes from
  // normalized coordinates). Returns ResourceExhausted if pools would exceed 12 MiB, InvalidArgument for bad sizes.
  Status configure(int outW, int outH, float renderScale);
  // Render one output frame into `out` (resized to the configured output if needed).
  void render(Frame& out, const RenderState& s);

  // Optional Unicode rasterizer (e.g. DirectWrite). Not owned. nullptr = built-in 5x11 bitmap font (ASCII + Latin-1).
  void setTextRasterizer(ITextRasterizer* r);
  // Pre-render + cache the bitmap for an overlay so the first live frame does not pay for layout (call on Publish).
  void prepareText(const TextOverlay& t, int compAspectNum = 0, int compAspectDen = 0);

  size_t poolBytes() const;             // bytes held by frame/polar/scratch pools (excludes text cache)
  size_t textCacheBytes() const;        // <= 8 MiB
  const RenderStats& stats() const;

  // Largest rect of aspect num:den centered in outW x outH (num/den <= 0 => whole output).
  static IRect fitRect(int outW, int outH, int aspectNum, int aspectDen);
  static constexpr size_t kPoolBudgetBytes = 12u << 20;
  static constexpr size_t kMaxTextOverlays = 4;

private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

} // namespace ss
