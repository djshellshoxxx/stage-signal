// MOD-06 renderer input (spec §13, REQ-S06/S10/D04/D07). Self-contained value description of ONE output frame.
// All pointers are borrowed const and must outlive the render() call. Floats here are converted to fixed-point
// once per frame; pixel math is integer-only so goldens match between x87 and SSE builds.
#pragma once
#include "core/types.h"
#include <string>
#include <vector>

namespace ss {

enum class FitMode : uint8_t { Fit, Crop, Stretch };            // aspect-fit (letterbox) / aspect-crop (cover) / stretch
enum class TransitionType : uint8_t { None, Cut, Crossfade };   // None: only scene A; Cut: only scene B; Crossfade: A->B by progress
enum class HAlign : uint8_t { Left, Center, Right };
enum class VAlign : uint8_t { Top, Middle, Bottom };

struct NormRect { float x = 0, y = 0, w = 1, h = 1; };          // normalized 0..1 in the target space (clamped)

constexpr float kParamDefault = -1.0e9f;                        // "use the scene registry default" sentinel
struct SceneParams {                                            // ranges/defaults: scene_registry.h
  float brightness = 1.0f;                                      // 0..1 scene-level
  float speed = 1.0f;                                           // 0.25..4 (multiplies scene time)
  float intensity = 0.5f;                                       // 0..1
  float hue = 0.0f;                                             // 0..1 palette rotation
  float extra[4] = {kParamDefault, kParamDefault, kParamDefault, kParamDefault};   // scene-specific (see registry)
};
struct SceneLayer {
  int scene = 0;                                                // registry index 0..11 (sceneIndexById)
  SceneParams params;
  uint32_t seed = 1;
  Ns timeNs = 0;                                                // scene-local time; animation is a pure function of it
};

struct MediaLayer {                                             // still/GIF/video frame supplied by MOD-07 (not owned)
  const Frame* frame = nullptr;
  FitMode fit = FitMode::Fit;
  NormRect crop;                                                // normalized SOURCE sub-rect applied before fitting
  float opacity = 1.0f;                                         // 0..1
};

struct CameraTreatments {                                       // spec §13; every field affects the camera layer only
  FitMode fit = FitMode::Crop;                                  // camera -> destination rect (also for split screen)
  NormRect crop;                                                // normalized source crop
  bool pip = false;  NormRect pipRect{0.66f, 0.66f, 0.30f, 0.30f};   // normalized composition rect
  bool mirror = false;                                          // horizontal flip
  float tintAmount = 0.0f; uint32_t tintRgb = 0x00FF8000;       // 0..1 lerp of each pixel toward tintRgb
  bool threshold = false;  float thresholdCutoff = 0.5f;        // luminance >= cutoff -> white else black
  bool posterize = false;  int posterizeLevels = 4;             // 2..16 levels per channel
  bool split = false;      bool splitCameraRight = true;        // camera occupies one half, scene the other
  int trailFrames = 0;                                          // 0/1 = off, 2..8 = bounded accumulation (nominal frames @25 FPS)
};
struct CameraLayer {
  const Frame* frame = nullptr;
  bool routed = false;                                          // deliberate publication: false => camera ignored entirely
  CameraTreatments t;
};

struct TextOverlay {                                            // published text (REQ-S06)
  std::u32string text;                                          // truncated to 4096 code points
  NormRect box{0.05f, 0.70f, 0.90f, 0.25f};                     // normalized composition rect the text lives in
  HAlign h = HAlign::Center; VAlign v = VAlign::Bottom;
  float sizeFrac = 0.07f;                                       // line height as fraction of composition height
  uint32_t color = 0x00FFFFFF;
  bool bold = false;  bool shadow = true;
  float alpha = 1.0f;                                           // fade 0..1
  int scrollPx = 0;                                             // vertical scroll in text-bitmap pixels, clamped to [0, textH-boxH]
};

struct OverlayOptions {                                         // approved overlays only (never secrets/paths)
  bool showTempo = false, showKey = false;                      // tempo/key badge with Estimated/Manual/Tap label + confidence
  const Frame* brandingImage = nullptr;                         // approved branding bitmap (opaque), bottom-right
  float brandingHeightFrac = 0.10f;
  std::u32string brandingText;                                  // approved text branding, bottom-left
  float alpha = 1.0f;
};

struct ShedKnobs {                                              // adaptive-quality knobs (spec §7), caller drives the ladder
  int trailFramesCap = 8;                                       // 0/1 disables trails (first thing shed)
  int particlePercent = 100;                                    // 10..100
  bool fallbackScenes = false;                                  // swap High-cost scenes for their registry fallback
  bool halfResTransition = false;                               // crossfade renders both scenes at half internal size
  // level 0 none; 1 trails off; 2 + particles 50%; 3 + particles 25%, fallbacks, half-res transitions
  static ShedKnobs forLevel(int level) {
    ShedKnobs k;
    if (level >= 1) k.trailFramesCap = 0;
    if (level >= 2) k.particlePercent = 50;
    if (level >= 3) { k.particlePercent = 25; k.fallbackScenes = true; k.halfResTransition = true; }
    return k;
  }
};

struct RenderState {
  Ns elapsedNs = 0;                                             // monotonic show time (trail decay, frame pts)
  uint32_t generation = 0;                                      // copied to Frame::generation
  uint32_t background = 0x00000000;                             // layer 0
  SceneLayer a, b;
  TransitionType transition = TransitionType::None;  float transitionProgress = 0;   // 0..1
  MediaLayer media;
  CameraLayer camera;
  std::vector<TextOverlay> texts;                               // first 4 used
  OverlayOptions overlays;
  float masterBrightness = 1.0f;                                // 0..1
  bool blackout = false;                                        // latched: output is exactly black this call
  bool disableStrobing = false;                                 // strobe-flagged scenes use smooth fallback + slew guard
  FeatureSnapshot features;
  float internalScale = 0.0f;                                   // 0 = configured scale; else clamped to (0.1 .. configured]
  ShedKnobs shed;
  bool refBorder = false;                                       // REQ-D04 reference border/corner test pattern at fit-rect edge
  int compAspectNum = 0, compAspectDen = 0;                     // composition aspect (0 = same as output); else aspect-fit in output
};

} // namespace ss
