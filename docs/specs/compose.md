# compose (MOD-06, compositing half)

Namespace `ss::compose`; library `ss_compose` (`ss_add_module(compose DEPS base model)`). Portable C++17, no JUCE/OS.
It does NOT depend on `render`: the scene is an `ss::PixelBuffer` handed in by the caller. Decoding (GIF/video/stills),
font rasterisation and camera capture are app responsibilities; compose only composites already-decoded pixels.

## Scope and requirements
REQ-D04 (aspect fit, 1 px reference border visible), D05 (no diagnostics on audience outputs unless enabled), D07 (rotation
and aspect recompute in normalised coordinates), D08 (visual blackout by the next admitted frame, exact restore), S06 (text),
S09 (camera publication is an explicit flag), S10 (treatments), S11 (camera loss); section 7 adaptive internal render scale;
section 11 overlay policy; section 13 composition order, camera treatments, text and media limits.

## Conventions
* Pixels: RGBA8, **premultiplied alpha**, sRGB SDR, tightly packed (`ss::PixelBuffer`). Valid pixels have c <= a; operators
  saturate so invalid input cannot wrap. All math is integer fixed point (`div255` rounding) => bit-identical on all platforms.
* Output frames are opaque (alpha 255): the background is always opaque and blackout writes (0,0,0,255).
* Sampling is corner aligned: first/last destination pixel = first/last source pixel, so a 1 px source border always lands on
  the outermost row/column of the fitted rectangle (REQ-D04). Fit (letterbox), Crop (cover), Stretch; Nearest or Bilinear.
* Rotation is clockwise degrees (0/90/180/270). The composition is built in a *logical* canvas (swapped w/h for 90/270) in
  normalised coordinates and rotated into the physical frame last; show content is never modified (REQ-D07).

## Composition order (fixed)
1. background colour (opaque)  2. scene A (fitted; with a transition B is rendered too and blended)  3. media layers (<=4)
4. camera layer (only if published and visible)  5. published text (policy `showText`)  6. approved overlays (policy)
7. master brightness (RGB scale, 1.0 = identity)  8. visual blackout (opaque black)  9. rotate into pooled output frame.
Steps 1-4 are rendered at the **internal** resolution; the result is upscaled (Nearest/Bilinear) to the logical size, then
5-8 run at full output resolution so text/labels stay crisp when the scale is lowered.
Idle (`FrameInputs::idle`, audio-loss policy) replaces steps 2-4 with the idle colour + logo (bitmap or outlined placeholder
with built-in-font label); text/overlays/brightness/blackout still apply.
Split-screen: the camera takes a pane (ratio 0.1..0.9, vertical or stacked, camera first/last, camera fit mode); the scene is
fitted into the other pane with the scene fit mode.

## Transitions
`Cut` (A at progress 0, B for >0), `Crossfade` (premultiplied per-channel mix, weight = round(p*255), 0 => A and 1 => B exactly),
`FadeThroughBlack` (A dims to pure black at p=0.5, then B brightens; RGB only, alpha kept). NaN progress => 0. At progress 1
only B is rendered (A is skipped).

## Camera layer (REQ-S09/S10/S11)
`CameraLayer` is prepared once for a maximum frame size. Per pushed frame: mirror -> posterize (2..16) -> threshold (0..1
luminance cutoff, Rec.709 weights, `luma >= cutoff` is white) -> tint (RGB colour, amount 0..1) -> motion trail
(`trailFrames` 0 or 2..8; accumulation buffer `acc = max(cur, floor(acc*(n-1)/n))`, so trails always fade out, bounded memory).
Treatments exist only inside the layer, so they can only affect the routed camera. Placement: Full, PIP (normalised rect),
Split. `published` is an explicit flag: unpublished layers are never composited; `preview()` still returns the processed frame.
Trail is the first thing shed: `Compositor::setQualityStep(1)` sheds trails, 2..4 lower the internal scale to 75/50/35 %.
Loss: the layer is bound to one `sourceId`; frames from any other id are rejected and counted; there is no API to pick another
camera. Loss is established by `markLost(now)` (device event) or when no frame arrived for `staleMs` (default 250 ms; loss time =
last frame + staleMs). From the loss time the last processed frame is held for `freezeMs` (clamped to <= 500) and then the layer
is Hidden or shows the placeholder colour + "NO CAMERA" label. A frame from the same source restores Live immediately.
Honest note: with stale detection the picture can stay on screen up to staleMs + 500 ms after the last good frame; the 500 ms
bound is measured from the moment loss is established. Use `markLost` for exact device-lost timing.

## Text (REQ-S06)
The show engine owns draft/preview/publish; `TextLayer::publish` takes a ready item: pre-rasterised premultiplied bitmap
(coverage mask tinted by `color`, or `colorBitmap` for emoji/own colours), accompanying UTF-8 `utf8` (well-formed, <= 4096 code
points, via `ss::utf8`), normalised centre x/y, `size` = rendered height / canvas height, duration (0 = hold, else 0.1..3600 s),
fade (clamped to duration/2), bounded scroll (one right-to-left pass, 2..120 s, default 8 s, no wrap). Max 4 active items
(`ResourceExhausted` otherwise); same id replaces. Over-wide static text is scaled down to fit. Bitmaps <= 4096 px per edge
and 64 MiB are checked before use. The app should rasterise at the final pixel size (bitmaps are only bilinear-scaled).
Built-in 5x7 ASCII font (`drawText`) for core labels; non-ASCII renders as '?'.

## Overlays and policy (REQ-D05)
`OutputPolicy{showText, showBpmKey, showBranding, showDiagnostics}` (from `OutputRoute` via `policyFromRoute`). Default policy:
text only. BPM/key label: `BPM 128 (Manual)`, `BPM 127.5 (Estimated)`, `BPM 120 (Tap)`, `Key Am (Estimated)`; unknown values draw
nothing (never a numeric 0). Branding = approved logo bitmap and/or <=31 ASCII chars. Diagnostics (<=4 lines) and the clip warning
are drawn only when `showDiagnostics`; lines that look like a path or a secret are replaced by `[redacted]` (best-effort heuristic,
and the overlay model has no path/secret field). Blackout hides overlays as well.

## Blackout (REQ-D08)
`setVisualBlackout(bool)` is an atomic flag (any thread). `compose()` samples it once at frame start, so the next admitted frame is
black. All other state (transition progress, text clocks, camera trail, loss state) keeps advancing, so clearing the flag yields
exactly the composition that would have been drawn had blackout never happened (tested against a parallel compositor). The control
path (UI, MIDI, lighting) never touches the compositor and is unaffected.

## Media timing helpers (decode not here)
`clampGifDelay` (<20 ms => 20 ms + `clamped` warning), `GifTimeline` (<=1000 frames, speed 0.25..4, loops 0 = infinite),
`VideoTimeline` (speed 0.25..2, loop), `sanitizeCrop`. All are pure functions of elapsed *media* time, never of render ticks.

## Limits and failure behaviour
Output/buffer edge <= 4096 and <= 64 MiB (checked before allocation); internal scale 0.25..1; pool 1..8 frames; media layers 4;
active text 4. Errors are `ss::Status` (InvalidArgument / ResourceExhausted), no exceptions, no secrets/paths in messages.
Pool exhausted => `compose` returns `!ok()` and counts `droppedFrames()` (never grows). No allocation after `prepare()` in
`compose`, `setRotation`, `setInternalScale`, `setQualityStep`, camera `pushFrame/update/configure`, text `draw` (counting-new test).

## Tests (`tests/unit/compose`, ~67 cases, ~2 s)
Exact 16x12 goldens for fit/crop/stretch, composition order, brightness, each transition, each treatment, split/PIP, rotations,
blackout + restore, idle; border visibility at 800x600, 1024x768, 1280x720, 600x800, 640x480, 400x300 (both samplers) and via
rotation; premultiplied bounds (exhaustive div255, over() reference sweep, saturation); frame pool bound; counting `operator new`
(zero allocations); determinism (hash sequence equality); camera loss with `FakeClock`; text limits/envelope/scroll; overlay policy.

## Status
* Done: everything listed above, built clean with `-Wall -Wextra -Wshadow` (gcc 13, clang) and tests green 3x.
* Partial/known gaps: (1) software raster only, no GPU path; performance on the 800 MHz BASE tier is NOT measured (no fixture,
  no benchmark here), only designed to be allocation-free and integer-only. (2) Camera colour matrix/range conversion is the
  app's job; frames must arrive as sRGB RGBA8. (3) Trail uses one accumulation buffer, not a ring of N historical frames
  (the spec allows either). (4) Redaction of diagnostics is a heuristic. (5) No HDR / colour management (deferred by spec).
  (6) Media layers draw one frame each; selecting the GIF/video frame from the timeline is the caller's job.
* Needs from others: `show` owns text draft/preview/publish and blackout state; `perf` decides the quality step (this module only
  exposes `setQualityStep`); `app` supplies decoded frames, text bitmaps, camera frames and the present/release loop.

## Integration API
```cpp
#include "ss/compose/compositor.hpp"
using namespace ss::compose;
Compositor comp;
comp.prepare(outW, outH, /*internalScale*/1.0f, /*poolFrames*/3, /*rotationDeg*/route.rotationDeg); // control path, allocates
CompositorConfig cfg; cfg.policy = policyFromRoute(route); cfg.sceneFit = FitMode::Fit; comp.setConfig(cfg);
CameraLayer cam; cam.prepare(camMaxW, camMaxH);          // once per routed camera
CameraLayerConfig cc; cc.sourceId = deviceId; cc.published = false; cam.configure(cc);
TextLayer text;  text.publish(item, nowNs);              // item.bitmap = shared_ptr<const PixelBuffer> from JUCE rasteriser
// camera thread/callback -> render thread: cam.pushFrame(deviceId, frame, nowNs); on device lost: cam.markLost(nowNs);
// render tick (render thread):
FrameInputs in; in.nowNs = clock.nowMonotonic();
in.scene = {&sceneA, &sceneBOrNull, kind, progress};     // scene pixels from module render
in.media[0] = {&decodedFrame, FitMode::Fit, {0,0,1,1}, {0,0,1,1}, 1.f}; in.mediaCount = 1;
in.camera = &cam; in.text = &text; in.overlay = &overlayInfo; in.masterBrightness = b; in.idle = audioLostIdle;
ComposedFrame f = comp.compose(in);                      // !f.ok() => pool exhausted, count a drop
if (f.ok()) { present(*f.pixels); comp.release(f); }     // release from the present thread when done reading
// control path (any thread): comp.setVisualBlackout(on);  // honoured by the next compose()
// render thread only: comp.setRotation(deg); comp.setInternalScale(s); comp.setQualityStep(step 0..4);
```
Rules: call every setter and `compose` from the render thread except `setVisualBlackout`/`visualBlackout` and `release`
(lock-free, any thread). `setQualityStep(1)` sheds camera trails on the next `compose`. Camera `state()` after `compose` is
`Live/Frozen/Hidden/Placeholder/NoSignal` for the UI. Duplicate outputs share one `compose` result; Independent outputs get their
own `Compositor` (own pool/policy). Pass media/camera/text bitmaps premultiplied; never pass diagnostics/paths/secrets expecting
them to be shown on audience routes.
