# Module Reference: compose

**Purpose.** The compositing half of MOD-06. It takes already-rendered scene frames, media frames (GIF or video frame chosen by the caller), a camera frame, text items and overlay info, and produces one output frame in a fixed order, with transitions and blackout. Decoding is not here: the app decodes and passes frames in.

**Depends on.** `base`, `model`.

**Note.** `compose` is in the repository, but it is not in the module list in `docs/ARCHITECTURE.md`. Its dependencies fit the allowed rules.

## Public headers

| Header | Contents |
|---|---|
| `compositor.hpp` | `Compositor`, `CompositorConfig`, `FrameInputs`, `ComposedFrame`, `MediaLayerInput`, `SceneInput`, `DrawParams`, `FitResult` |
| `camera.hpp` | `CameraLayer`, `CameraLayerConfig`, `CameraTreatments`, `CameraPlacement` (Full, Pip, Split), `CameraState`, `LossPolicy` (Hide, Placeholder), `SplitConfig` |
| `text_layer.hpp` | `TextLayer`, `TextItem` |
| `overlay.hpp` | `OverlayInfo`, `OutputPolicy` |
| `transition.hpp` | Transition helpers |
| `media_timing.hpp` | `GifTimeline`, `GifDelay`, `GifPosition`, `VideoTimeline`, `VideoPosition` |
| `frame_pool.hpp` | `FramePool` (1 to 8 frames) |
| `font5x7.hpp` | Built-in 5 x 7 font |
| `limits.hpp` | Output and buffer limits |
| `pixel.hpp`, `raster.hpp`, `overlay.hpp`, `text_layer.hpp` | Pixel formats and drawing helpers |

Composition order is fixed in `compose.md` (scene, media, camera, text, overlays, blackout).

## Tests

`tests/unit/compose`: 67 doctest cases across `test_alloc.cpp`, `test_camera.cpp`, `test_compositor.cpp`, `test_font_overlay.cpp`, `test_raster.cpp`, `test_text.cpp` and `test_transition_media.cpp`.

## Status (from `docs/specs/compose.md`)

- Done: everything in the spec, built clean with `-Wall -Wextra -Wshadow` on gcc 13 and clang, tests green three times.
- Software raster only. BASE (800 MHz) performance is not measured. The code is designed to be allocation free and integer only.
- Camera colour matrix and range conversion is the app's job. Frames must arrive as sRGB RGBA8.
- The trail effect uses one accumulation buffer, not a ring of past frames.
- Redaction of diagnostics is a heuristic.
- No HDR and no colour management.
- Media layers draw one frame each. Choosing the frame from the timeline is the caller's job.

## Limits and failure behaviour

- Output and buffer edge at most 4096 and 64 MiB, checked before allocation.
- Internal scale 0.25 to 1. Pool 1 to 8 frames. Four media layers. Four active text items.
- A full pool returns an error and counts dropped frames. It never grows.
- No allocation after `prepare()` in the compose, rotation, scale, quality, camera and text paths.

## Needs from others

- `show` owns text draft, preview, publish and blackout state.
- `perf` decides the quality step. This module only exposes `setQualityStep`.
- The app supplies decoded frames, text bitmaps, camera frames and the present and release loop.
