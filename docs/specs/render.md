# MOD-06 render: procedural scenes, effects, fit helpers

Namespace `ss::render`. Depends on `base`, `model`. The layer compositor is the separate `compose` module; this
module only produces single-scene frames, post effects and aspect-fit helpers.

## Scope and REQ ids

| REQ | What this module provides | Tests |
|---|---|---|
| REQ-D04 | `fitRect`, `blitFit`: aspect fit into an 800x600 target, opaque-black letterbox, reference borders survive the blit | `REQ-D04: aspect fit...`, `REQ-D04: reference borders...` |
| REQ-D07 | `rotatedSize`, `normToPixels`, `blitFit(rotation)`: 4:3, 16:9, portrait and 90/180/270 rotation recomputed from normalised geometry; content untouched | `REQ-D07: rotation and resolution...` |
| REQ-S12 | Scenes that flash report `usesStrobe()`; with `strobingDisabled` they render a bounded non-flashing fallback; BloomPulse narrows its pulse range | `REQ-S12: ...` (2 cases), bloom case |
| REQ-P06 / section 7 | No allocation in `render()`/`applyEffect()`/`blitFit()` after `prepare()`; animation from elapsed time; integer fast paths; relative cost estimates | `REQ-P06: render() performs zero heap allocations...`, effects allocation case |
| Section 13 | The 12 original default-pack scenes: seeded state, parameter ranges, cost, strobe flag, fallback, golden-frame fixtures | golden + property cases |
| DEC-13 | PrismMirror and BloomPulse effects | effects cases |

S06 is not applicable to this module.

## Public API

```
ss/render/scene.hpp     AudioVisualInput, SceneInput, SceneRenderer, makeScene(SceneKind)
ss/render/effects.hpp   EffectScratch, applyEffect(EffectKind, amount, SceneInput, PixelBuffer&, EffectScratch&)
ss/render/fit.hpp       RectI, FitMode, fitRect, rotatedSize, normToPixels, blitFit
ss/render/raster.hpp    Canvas + fill, fillRect, gradientV/H, lineAA, fillCircleAA, ringAA, glowDisc, putAdd/putCover
ss/render/fixed.hpp     integer helpers (sinQ15, atan2Q16, isqrt64, hsv, phaseQ16, toQ8)
```

The first two headers are the cross-module contract and are used exactly as agreed with the other workers.

## Data flow and conventions

* Output frame: `ss::PixelBuffer`, RGBA8, premultiplied, sRGB, tightly packed. Scene frames are always opaque
  (alpha 255) with a black background, so "over" blending keeps alpha at 255.
* `prepare(w, h, seed)` allocates everything (frame-size LUTs, particle pools). `render()` never allocates. The `out`
  buffer must be exactly the prepared size, otherwise it is cleared to black and nothing else happens. A buffer whose
  vector is shorter than `w*h*4` is ignored (never written).
* `SceneInput.seed`: 0 or equal to the prepared seed has no effect; any other value re-derives the seeded state
  without allocating. State is a pure function of the seed.
* All animation derives from `SceneInput.timeNs` (`phaseQ16(timeNs, speed, periodMs)`); `dtSec` is never integrated.
  Skipping frames cannot change speed, and rendering the same time twice (or on a fresh instance) gives identical
  bytes. One caveat inherent to a stateless time function: changing `speed` mid-scene rescales the phase, so motion
  jumps at the moment `speed` changes (callers should treat speed changes like any other scene parameter edit).
* Null `features`, `audio` or `params` are allowed. Without `audio`, a gentle time-based idle animation is shown
  (it freezes at speed 0). Without `features`, bands are derived from the spectrum and a soft idle pulse is used.
* Parameter ranges (values outside are clamped, NaN counts as 0):

| Param | Range | Meaning common to all scenes |
|---|---|---|
| brightness | 0..1 | master level. 0 returns an exactly black frame immediately |
| speed | 0..8 (1 = nominal) | time scale for every phase; 0 freezes the scene |
| intensity | 0..1 | scene specific "amount" (see catalogue) |
| hue | any, in turns (0..1 wraps) | base hue offset |
| saturation | 0..1 | 0 yields pure greys on every scene |

* Determinism across gcc/clang/MSVC: scenes use integer arithmetic only. Floats from the show/audio side are
  converted by multiplying with a power of two (exact) and truncating. The only floating point call is `std::sqrt`,
  used as a seed for `isqrt64`, whose integer correction loops make the result exact on every FPU/flag set.
  Right shifts of negative ints are relied on to be arithmetic (true on all three compilers, C++20 semantics).

## Scene catalogue

Cost is relative CPU time per frame at equal resolution, ColorField = 1.0, measured on the dev container (noisy
2-core machine, min of several runs). It is an admission hint, not a guarantee; the real BASE numbers come from the
benchmark tool. The test `benchmark (prints only)` prints ms/frame at 320x240 and asserts nothing.

| Scene | Look | Intensity does | Audio use | Cost | Strobe |
|---|---|---|---|---|---|
| SpectrumBars | 32 gradient bars with caps | bar gain | each bin drives one bar; onset lifts value | 0.45 | no |
| MirrorBars | 2x16 bars mirrored left/right and up/down from the centre | bar gain | bins 0..31 paired; low bins at the centre | 0.25 | no |
| WaveformRibbon | three additive polylines of the waveform with slow drift | deflection | waveform, onset brightens | 0.45 | no |
| GeometricOrbit | 5..7 bodies on elliptical orbits joined by a polygon, centre ring | body size, line level | low band pulses radii, high band body size, onset glow | 1.2 | no |
| RadialPulse | expanding palette rings from the centre | ring level | low band gain, onset core glow | 1.4 | no |
| ParticleField | 48..160 drifting glow particles (closed-form position from time) | particle count | high band size, onset twinkle | 0.8 | no |
| NeonGrid | perspective synthwave grid scrolling toward the viewer, sun at the horizon | line level | low band brightens grid and sky, mid band sun | 2.0 | no |
| Tunnel | checker tunnel with spiral twist | spiral amount | low band lightens the dark checks | 2.0 | no |
| ColorField | four-corner bilinear colour wash | corner hue spread | low band value, onset lift | 1.0 | yes |
| Starburst | rotating rays from the centre | ray width | low band width, mid band length, onset core | 2.7 | yes |
| PlasmaLite | four-term integer sine plasma through a hue palette | palette span (contrast) | low band value, onset shifts palette | 1.1 | no |
| LissajousScope | parametric a:b curve (a, b from the seed) with halo, waveform wobbles the radius | wobble depth | low band size, waveform radius, onset brightness | 1.2 | no |

Scenes with seed-dependent state: GeometricOrbit (body count, periods, orbit shapes), ParticleField (pool),
Tunnel (segment count), ColorField (corner periods), Starburst (ray count), PlasmaLite (spatial frequencies),
LissajousScope (a, b). The remaining scenes ignore the seed by design.

SpectrumBars and MirrorBars have no time term while real audio is supplied (they are functions of the bins);
they animate with time only in idle mode.

### Strobe flag and fallback (REQ-S12)

Only ColorField and Starburst may flash: on a real `FeatureSnapshot::onsetFired` event they apply a full-frame lift
(a 250 ms linear-decay envelope computed from elapsed time, never from frame count). `usesStrobe()` is a
capability flag and is true for these two regardless of settings. With `SceneInput.strobingDisabled`:

* ColorField: the full-frame add is capped at 16/255 (about 6%) and the onset value lift is removed.
* Starburst: no full-frame white add; the flash only widens and lengthens the rays slightly.

Tests check that the flash frame is a large luminance jump with strobing enabled, stays below 7% of full scale with
it disabled (including frame to frame across the decay), and that the 10 non-flashing scenes are byte-identical with
the switch on or off. Idle animation never flashes (the flash term only comes from real onset events).

## Effects

`EffectScratch` is caller owned; `prepare(w, h)` allocates (limit 4096x4096, checked before allocation, about 1 MiB
at 400x300 including the frame copy). `applyEffect` never allocates and leaves the frame untouched when the amount
is 0 / NaN / negative, the kind is None, or the scratch is not prepared for that exact size. One scratch serves
both effects.

**PrismMirror** (`EffectKind::PrismMirror`): kaleidoscope. Per pixel the polar angle (from a LUT) plus a slow rotation
(one turn per 40 s at speed 1) is folded into a mirrored wedge of `SceneParams::prismSegments` segments (clamped
2..16), and the colour is sampled at the folded position (nearest). The sampled wedge sweeps the source at half the
rotation speed. `amount` blends linearly with the source (Q8); 1 is the full fold. Cost about 2.5x ColorField.

**BloomPulse** (`EffectKind::BloomPulse`): 4x4 box downsample, bright pass (threshold 96), two separable 5-tap box blurs
at quarter resolution, bilinear upsample, additive composite clamped to alpha (premultiplied-safe, never darkens).
Strength = `amount * pulse`, pulse = 64 + 0.4 * low band + 0.4 * onset (onset forced to full on `onsetFired`),
glow gain up to 2x. With `strobingDisabled` the pulse is clamped to 64..112 so it cannot flash. Cost about 4.5x
ColorField at 320x240, bounded and independent of content.

*Attack/release (NEW-02, 28 Oct 2026).* The pulse is slew-limited per second of `input.timeNs`: it rises at most 1024 Q8/s
(full range in 0.25 s) and falls at most 512 Q8/s (0.5 s), state kept in `EffectScratch` (`pulseQ8_`). Reason: before this a
solid grey frame with an onset on every frame swung mean relative luminance by 0.29 and produced 15 / 30 / 60 flashes per
second at 30 / 60 / 120 fps, breaking acceptance 2 of NEW-02 (found by the new `NEW-02:` flash-trace tests, which fail on
the old code). Re-seed rule: if `timeNs` does not advance (same instant twice, or scene time restarted) the pulse snaps to the
instantaneous target, so stateless callers and goldens are unchanged. Measured by `worstFlashesPerSecond` (WCAG 2.3.1:
opposing >= 0.10 relative-luminance changes, darker state < 0.80) on whole-frame mean luminance at 30/60/120 fps and
sub-sampled to 10 Hz; locked frames never cross the 0.10 threshold. Limits: whole-frame mean is a proxy for the WCAG
25 %-of-screen area rule; no photosensitivity test with people or a certified analyser was done.

## Failure behaviour

No exceptions, no asserts for runtime conditions. Wrong-size or too-short output buffers are cleared or ignored,
unprepared scratch is a no-op, non-finite parameters clamp to 0, `blitFit` returns false for empty buffers.
`makeScene` returns nullptr for Image/Gif/Video/Count (media scenes belong to the `media`/`compose` modules).

## Tests (doctest, `tests/unit/render`)

Golden frames (160x120, fixed seed/time/audio, FNV-1a 64 of the RGBA bytes in `golden/frames.txt`, 12 scenes + 4
strobe variants + 4 effect frames); determinism (twice, across instances, after frame skips); time/speed behaviour;
non-black, opaque, brightness 0 gives black; hue, saturation 0 gives grey, brightness monotonic, intensity changes the
frame; NaN/inf/huge parameter fuzz; guard canaries over 7 frame sizes including 1x1; strobe flag and fallback;
audio reactivity; seed behaviour; zero allocations with a counting `operator new`; raster primitives; fit helpers;
effect properties; prints-only benchmark. Regenerate goldens with `SS_UPDATE_GOLDEN=1 ./test_render`.

Verification performed: gcc (-O3), clang with `-ffp-contract=fast`, and an ASan+UBSan build all produce the committed
hashes. MSVC was not available here: the hashes are expected to match because no float result is hashed, but this
has not been run on MSVC.

## Status

* Done: 12 scenes, 2 effects, raster primitives, fit/rotation helpers, golden/property/allocation/benchmark tests.
* Costs are relative estimates from a noisy shared container, not BASE (800 MHz) measurements; nothing here claims
  BASE qualification. At 400x300 the heaviest scene is about 3 ms on the dev machine; BASE numbers need the perf tool.
* Not done: MSVC run of the golden test; a sub-pixel (bilinear) PrismMirror sample (nearest is used for cost);
  media scenes (out of scope); GPU paths (not part of the baseline).
* Behaviour to know: speed changes mid-scene jump the phase (stateless time function); bar scenes are pure functions
  of the audio bins.

## Integration API

```cpp
#include "ss/render/scene.hpp"
#include "ss/render/effects.hpp"
using namespace ss::render;

// once per scene change / resolution change (not real time):
auto scene = makeScene(def.kind);            // nullptr for Image/Gif/Video -> use media/compose
scene->prepare(w, h, def.seed);              // e.g. 400x300 internal frame
EffectScratch fxScratch; fxScratch.prepare(w, h);   // one per render thread

// every frame (real time, no allocation):
AudioVisualInput av;                         // app fills from ss::audio: 32 log bins 0..1, 256 waveform samples
SceneInput in;
in.timeNs = sceneElapsedNs;                  // elapsed time of THIS scene instance, monotonic
in.dtSec = dt;  in.features = &snap;  in.audio = &av;  in.params = &def.params;
in.seed = def.seed;                          // optional; reseeds only if it differs from prepare()
in.strobingDisabled = showSnapshot.strobingDisabled;   // REQ-S12, always forward the live value
scene->render(in, frame);                    // frame must be w x h
applyEffect(EffectKind::PrismMirror, def.params.effectAmount[0], in, frame, fxScratch);
applyEffect(EffectKind::BloomPulse,  def.params.effectAmount[1], in, frame, fxScratch);

// admission / UI: scene->costEstimate() (ColorField = 1), scene->usesStrobe() -> mark the SceneDef strobe flag.
// output fit (REQ-D04/D07): blitFit(frame, outputBuffer, FitMode::Contain, route.rotationDeg);
//   fitRect(w, h, vw, vh, mode) gives the destination rectangle for hit-testing / overlays.
```

Rules for the caller: one `SceneRenderer` and one `EffectScratch` per render thread; call `prepare` again (outside
real time) when the internal resolution changes; pass the same `timeNs` basis for every layer of a composition;
set `strobingDisabled` from the live show state on every frame. Effects are order-sensitive: apply Prism before Bloom.
Bloom reads `FeatureSnapshot::band[0]`, `onsetStrength`, `onsetFired`.
