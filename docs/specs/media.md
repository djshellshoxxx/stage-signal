# Module spec: media (MOD-07 rules)

Namespace `ss::media`, directory `core/media`, depends on `base`, `model`. Portable C++17, no I/O except the
`ss_asset_check` CLI, no threads, no decoders. This module holds the **rules** for media: what may be loaded, how
much memory it may cost, how animation time maps to frames, how decoder failure is contained, and what the asset
manifest must say. Actual decoding (libpng, libjpeg, ffmpeg, camera capture) belongs to the app layer behind
`IMediaWorker`.

## Scope and requirement ids

| Id | What this module provides |
|---|---|
| REQ-S07 | Validators that run on header numbers before any allocation: still size and decoded bytes, BASE caps, GIF frame cap and cache budget, speed ranges, text length, source admission. |
| REQ-S08 | `IMediaWorker` + `MediaSupervisor`: decode failure, hang or crash of the worker is contained; the operator sees a placeholder and the previous scene stays. |
| REQ-S15 | Asset manifest model, validator, and the `ss_asset_check` release gate. |
| REQ-X03 | Fault containment for media: kill/restart with backoff, bounded work per `pump()`, no exception or crash path into the show clock. |

## Limits (spec section 13), all in `Limits` (`format.hpp`)

* Stills: width and height each <= 4096, decoded RGBA8 <= 64 MiB. BASE tier: <= 800x600 live, combined decoded cache
  <= 8 MiB (`DecodedCacheBudget::forTier`). A larger BASE still gets `needsDownscale` (preprocess off the live path),
  never a live resize.
* GIF: <= 1000 frames (the block walker stops at 1001 and reports `frameCapExceeded`), streamed cache <= 64 MiB on
  STANDARD+, at least 3 frames resident (current, restore-previous, next). BASE only with the lightweight-GIF
  benchmark flag.
* Speeds: video 0.25..2.0x, GIF 0.25..4.0x. NaN, infinity and out-of-range are rejected (`validateSpeed`), UI sliders use `clampSpeed`.
* Text: valid UTF-8, <= 4096 code points, no NUL; <= 256 messages per show.
* Admission (`admitSources`): STANDARD+ admits one video plus one camera; BASE admits a camera only after the
  320x240/10 FPS spike benchmark qualified this machine. Every refusal has a human-readable reason line.

## Public API

* `sniff(bytes)` identifies PNG, JPEG, BMP, GIF, MP4, MOV, MKV **by magic bytes only**. SVG, HTML, XML, script text
  and anything unknown are rejected with `SniffReject`. `checkDeclaredFormat` compares an extension/declared type with
  the sniffed one and fails on mismatch. Extensions are never trusted.
* `readImageHeader` (PNG IHDR, JPEG SOF scan, BMP header) and `parseGif` (bounded block walker: frame count, delays,
  disposal, NETSCAPE loop) read only headers, never pixel data, and bound their own work.
* `validateStill`, `validateGif`, `validateVideo`, `validateText`, `validateCrop`, `computeFit`, `fitWithin`,
  `normalizeGifDelays` (zero or < 20 ms delay clamps to 20 ms with a warning, matching browser behaviour).
* `GifTimeline(delaysMs, loopCount).frameAt(elapsedWallNs, speed)`: frame index from the show/media clock, never from
  a wall-clock sleep. Loop semantics: `-1` = no NETSCAPE block = play once, `0` = forever, `n` = n extra plays.
  Speed scales elapsed time, so 0.25x..4x never changes frame order.
* `loopPosition` for video position and loop end.
* `IMediaWorker` (non-blocking `submit/poll/kill/restart/alive`), `FakeMediaWorker` (modes Succeed, Fail, Hang, Crash,
  WrongSize) and `MediaSupervisor`.
* Manifest: `parseManifestJson`, `validateEntry/validateManifest`, `verifyManifest(manifest, IAssetSource&, options)`,
  `isValidSpdxExpression`, `isSafeRelativePath`, `isSha256Hex`.

## Data flow

1. Import or cue: app reads at most a few KiB, calls `sniff`, `checkDeclaredFormat`, header reader, then
   `validateStill/Gif/Video`. A failing plan never reaches an allocator.
2. The app reserves the plan's decoded bytes via `MediaSupervisor::request` (`DecodedCacheBudget::tryReserve`); the
   worker (separate process or thread owned by the app) decodes the poster frame.
3. The control thread calls `pump()` each tick (at most `maxEventsPerPump` events). `view(mediaId)` returns the real
   frame when Ready, otherwise the placeholder (128x128 checker with a red frame, "media unavailable").
4. A scene switch asks `decideSwitch(current, next)`: `Proceed`, `KeepCurrent` (next failed or still pending and the
   current scene is valid: stay, tell the operator) or `UsePlaceholder`.

## Failure behaviour

* Decode failure, wrong-size output (worker exceeded the validated target) or hang (pending > `decodeTimeoutNs`)
  marks the slot Failed with a sanitized reason (no file content, no absolute path), releases its cache reservation,
  and kills the worker if it hung. Restart uses backoff 500 ms and gives up after 5 consecutive restarts until the
  operator calls `retryWorker()`.
* A failing media item never alters the time base: nothing here advances or stops a clock.
* Manifest findings are data, never exceptions. `ss_asset_check` exit codes: 0 all good, 1 findings (missing license,
  SPDX syntax, hash mismatch, absent file, unlisted shipped file, missing provenance or strobe metadata), 2 usage or
  unreadable manifest.

## Asset manifest (section 13)

Per entry: id, version, type, relative safe path, creator, original URL (or `isOriginalWork`), retrieval date,
license text or pointer, SPDX expression, copyright, attribution text/location, commercial-use and modification
permission (explicit Yes/No, not Unset), modified flag, derivative provenance, original and distributed SHA-256,
width/height/duration/size, `flash` metadata (declared; max flashes per second > 0 when it flashes), permitted tiers.
SHA-256 uses `ss::Sha256`. Camel-case JSON, schema 1.

## Tests (`tests/unit/media/test_media.cpp`, 33 cases, doctest, no files besides temp manifest, no ffmpeg)

Names carry the REQ ids (REQ-S07, S06 text, S08, X03, P05 queue bounds, S15): sniff allowlist, active-content
rejection and renamed files; image header dimensions; the 4096-per-side and 64 MiB limits from numbers only; BASE
800x600 and the shared 8 MiB cache; GIF block parsing, the 1000-frame cap, the 64 MiB stream cache, 20 ms delay
clamping, media-clock timing, speed and loop counts; speed ranges; video loop position; video import by tier; text
limit; admission matrix; crop/fit geometry; worker success, failure with KeepCurrent, placeholder substitution, crash
with restart backoff, fail-fast while down, hung decoder kill, oversize reply rejection, bounded queue and cache,
restart give-up until the operator retries; manifest completeness, per-field enforcement, SPDX syntax, JSON parsing,
absent/mismatched/unlisted files; and an end-to-end `ss_asset_check` run via `std::system` (executable path from the
compile definition `SS_ASSET_CHECK_EXE`) checking exit codes 0 and non-zero.

## Status (honest)

Complete and tested: everything listed above under Public API, including the CLI.

Partial or absent:
* **No decoders.** PNG/JPEG/BMP/GIF/video pixel decoding, camera capture, the real out-of-process worker and its IPC
  are the app's job; only the interface, a fake, and the supervisor logic exist. The supervisor has never run
  against a real crashing decoder.
* Header readers are intentionally minimal: they reject exotic but legal variants (e.g. progressive JPEG with
  unusual marker order beyond the bounded scan) rather than guess. Rejection is safe; acceptance of a header says
  nothing about the pixel data being decodable.
* Video validation uses caller-supplied `VideoInfo` (from an ffprobe-style probe or a library); no container parser here.
* The tier cache numbers are enforced by accounting, not by measuring real memory.
* STANDARD+ one-video-plus-one-camera is an admission rule; the benchmark that sets the BASE qualification flags is
  owned by the perf module.
* The manifest CLI checks metadata presence and syntax; it cannot judge whether a stated license is *true*.
* Unit tests only (doctest). No integration, fuzz or soak test was run; the header readers deserve a fuzzer.

## Integration API

```
#include "ss/media/{format,probe,validate,manifest,worker}.hpp"       // link ss::media
// 1. Import:   auto s = ss::media::sniff(bytes);  checkDeclaredFormat(ext, s);
//              readImageHeader(data,n,s.format) | parseGif(data,n)    -> validateStill/validateGif(info, tier, &budget)
//              validateVideo(VideoInfo{...}, tier); validateText(utf8); validateSpeed(SpeedKind::Gif, v)
// 2. Admission: admitSources(tier, {videos,cameras,gifs, baseCameraQualified, baseGifQualified}) -> reasons[] for UI
// 3. Decoding: implement IMediaWorker (submit/poll/kill/restart/alive, NEVER blocking); tests use FakeMediaWorker.
//              MediaSupervisor sup(worker, clock, DecodedCacheBudget::forTier(tier));
//              sup.request({mediaId, format, relPath, targetW, targetH, reserveBytes});   // from the *Plan structs
//              each control tick:  sup.pump();
//              render:   MediaView v = sup.view(mediaId);  // pixels never null: real frame or placeholder
//              cue:      switch (sup.decideSwitch(currentId, nextId)) { Proceed | KeepCurrent | UsePlaceholder }
//              unload:   sup.unload(mediaId);   operator retry after 5 failed restarts: sup.retryWorker();
// 4. Animation: ss::media::GifTimeline tl(plan.timing.delaysMs, plan.loopCount);
//              frame = tl.frameAt(mediaElapsedNs, speed, &finished);   // use the media/show clock, not wall time
//              Ns p = loopPosition(elapsed, speed, durationNs, loop, &ended);
// 5. Fit:      computeFit(srcW,srcH,dstW,dstH, FitMode::Fit|Crop|Stretch) -> {src,dst} rectangles; validateCrop(CropRect)
// 6. Manifest: parseManifestJson(text); validateManifest(m); verifyManifest(m, IAssetSource&, {requireAllFilesListed})
//              implement IAssetSource { sha256Hex(rel,&size); listFiles(); }  (use ss::Sha256 streaming)
// 7. Release gate (CI):  ss_asset_check --manifest <assets.json> --root <dir> [--allow-unlisted]
//              exit 0 all verified, 1 absent/mismatched/incomplete entry (findings printed), 2 usage/unreadable manifest
// Limits constants: ss::media::Limits::{kMaxStillDim,kMaxStillDecodedBytes,kBaseDecodedCacheBytes,kMaxGifFrames,...}
// Errors are ss::Result/Status with ErrorCategory::{InvalidArgument,ResourceExhausted,DecodeFailure,...}; texts are
// operator-safe (no file content, no absolute paths). The placeholder is MediaSupervisor::makePlaceholder().
// Not provided: decoders, camera capture, process spawning, video container parsing.
```
