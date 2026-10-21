# Module Reference: render

**Purpose.** Procedural scenes, the two effects, raster primitives, and fit and rotation helpers (spec MOD-06, render half). Software only, CPU raster. It draws into a caller-owned pixel buffer and never allocates after `prepare()`.

**Depends on.** `base`, `model`.

## Public headers

| Header | Contents |
|---|---|
| `scene.hpp` | `SceneRenderer`, `makeScene(SceneKind)`, `SceneInput`, `AudioVisualInput` |
| `effects.hpp` | `EffectScratch`, `applyEffect(EffectKind, amount, SceneInput, PixelBuffer&, EffectScratch&)` |
| `fit.hpp` | `RectI`, `FitMode`, `fitRect`, `rotatedSize`, `normToPixels`, `blitFit` (rotation 0, 90, 180, 270) |
| `raster.hpp` | `Canvas`, fills, gradients, anti-aliased lines and circles, glow, additive and cover puts |
| `fixed.hpp` | Integer helpers: sine (Q15), atan2 (Q16), integer square root, HSV, phase, Q8 conversion |

The first two headers are the cross-module contract. Other workers use them as agreed.

## Scenes

Twelve generated scenes: SpectrumBars, MirrorBars, WaveformRibbon, GeometricOrbit, RadialPulse, ParticleField, NeonGrid, Tunnel, ColorField, Starburst, PlasmaLite, LissajousScope. The kinds Image, Gif and Video exist in the enum, but media scenes are out of scope for this module. See [Scenes and Effects](Scenes-and-Effects) for the full table.

Only ColorField and Starburst can flash, and only on an onset event. `usesStrobe()` reports the capability. `strobingDisabled` switches them to a bounded fallback.

## Effects

- **PrismMirror.** Kaleidoscope fold, 2 to 16 segments (default 6), nearest sample. Amount 0 is pass-through.
- **BloomPulse.** Downsample, bright pass, blur, upsample, additive composite. Pulse is clamped to 64 to 112 when strobing is disabled.

## Tests

`tests/unit/render`: 28 doctest cases in `test_scenes.cpp`, `test_effects.cpp`, `test_raster_fit.cpp` and `test_alloc.cpp`. Golden frames are in `tests/unit/render/golden/frames.txt`.

## Status (from `docs/specs/render.md`)

- Done: 12 scenes, 2 effects, raster primitives, fit and rotation helpers, golden, property, allocation and benchmark tests.
- Costs are relative estimates from a noisy shared container. Nothing here claims 800 MHz (BASE) qualification.
- Not done: an MSVC run of the golden test, sub-pixel (bilinear) PrismMirror sampling, media scenes, and any GPU path.
- Behaviour to know: a speed change mid-scene jumps the phase, because the time function is stateless. Bar scenes are pure functions of the audio bins.

## Limits

- Effect scratch limit 4096 x 4096, checked before allocation.
- The benchmark tool that gives real BASE numbers is not built.
