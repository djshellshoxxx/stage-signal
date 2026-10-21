# Module `dynamics` (MOD-03 dynamics + recording audio path)

Namespace `ss::dynamics`, depends on `base` only. Headers in `core/dynamics/include/ss/dynamics/`, tests in
`tests/unit/dynamics/`. All numbers below are measured by the tests unless marked "design".

## Scope and REQ ids
| REQ | What this module provides |
|---|---|
| REQ-R02 | Compressor + limiter, on by default; bypass, settings and activity exposed (`metadataJson()`). |
| REQ-R03 | `DelayCompensation`, `ClockMap`, `DriftController`, `LinearResampler`: the logic that lets A/V PTS be corrected. Flash-click sync on real devices is NOT covered (needs hardware). |
| REQ-R05 | 24-bit PCM WAV segmented writer, header finalisation, `recoverWav`. |
| REQ-R04 | Sink failure returns `DiskFull`, earlier completed segments stay valid, active segment is repairable. |
| REQ-A10 | Raw input near-full-scale / possible-clipping flags are separate from limiter-controlled output meters. |
| REQ-P05 | No allocation or locks in `process()`; staged `prepare()`; bounded writer memory. |
| Section 8 | Gap policy: <=250 ms gap becomes labelled silence, repeated or longer gaps stop. |

## Parts and public API
1. `processor.hpp` `DynamicsProcessor`
   - `prepare(DynamicsConfig) -> Result<PrepareInfo>` (control thread; validates before allocating; stages an engine).
   - `setParams(DynamicsParams) -> ParamReport` (any thread; clamps NaN/out-of-range, reports which fields).
   - `process(in, out, frames) -> bool` (audio thread; interleaved float; `false` until an engine was adopted).
   - `telemetry()`, `resetHolds()`, `activeInfo()`, `metadataJson()`.
2. `wav_writer.hpp` `WavSegmentWriter`, `IByteSink` (`MemorySink`, `FileSink`), `ISinkFactory` (`MemorySinkFactory`,
   `FileSinkFactory`), naming helpers. `wav_reader.hpp`: `parseWavHeader`, `readWav` (verification only), `recoverWav`,
   `recoverWavFile`.
3. `sync.hpp`: `ClockMap`, `DriftController`, `DelayCompensation`, `LinearResampler`.
4. `truepeak.hpp`: `TruePeakMeter` (4x polyphase METER only).

## Signal path and parameters (section 12 table, enforced by `clampParams`)
`input -> [sanitise] -> [raw input meters] -> [mono->stereo route] -> trim -> compressor -> makeup -> limiter -> output meters`,
with a parallel dry path (delayed by the same lookahead) crossfaded in by bypass.

| Parameter | Default | Range | Notes |
|---|---|---|---|
| trim | 0 dB | -24..+12 | digital only |
| threshold | -12 dBFS | -36..0 | applied to the trimmed max-channel peak |
| ratio | 3:1 | 1..10 | |
| attack / release | 10 / 150 ms | 0.5..100 / 20..2000 | time constants (63%) of a one-pole smoother on gain reduction in dB |
| knee | 6 dB | 0..12 | soft knee, quadratic, centred on threshold |
| makeup | 0 dB | 0..12 | no auto makeup |
| limiter ceiling | -1 dBFS (sample peak) | -12..-0.5 | |
| limiter release | 100 ms | 20..500 | exponential |
| lookahead (config) | 5 ms | 1..20 | structural: change via `prepare()` only |

- Compressor detector: max over channels of |x|, 4 ms sliding-window peak hold (O(1) monotonic deque), level in dB, static
  curve, then attack/release smoothing of the gain reduction in dB. Both channels share one gain (balance preserved).
  Steady-state error is exactly the static curve for tones >= 250 Hz (window covers a full cycle). Below that the gain
  modulates within a cycle, as with any peak detector with a short window (design limit, documented, not tested).
- Limiter: delay line of `L = round(lookahead * fs)` frames. Per input frame: target gain `min(1, ceil/peak)`, sliding minimum over `L+1` targets,
  instant attack / exponential release stage, then a box average over `L+1` samples (exact re-sum every window, no drift).
  Because the box window only contains values that are <= every target covering the delayed sample, the delayed sample
  can never exceed the ceiling (up to float rounding). The ceiling is therefore a hard sample-peak bound from sample 0
  (the first `L` outputs are silence = "documented startup"). Measured: output peak <= ceiling + 0.1 dB on all vectors.
- Processing delay = `L` frames, identical for wet and dry paths, reported in `PrepareInfo`/telemetry. Measured equal to
  the actual impulse delay at 44.1 and 48 kHz (221 and 240 frames at 5 ms).
- Smoothing: trim, threshold, ratio, knee, makeup and ceiling glide with a 5 ms one-pole (no zipper steps; tested). Attack/release
  coefficients are recomputed per block. Bypass ramps linearly over 10 ms; the compressor/limiter keep running while bypassed
  so un-bypass has no state jump. With bypass fully on, output equals input delayed by `L` bit-exactly. **Bypass removes
  limiter protection** (dry path is unlimited).
- Robustness: NaN/Inf inputs become 0 (counted in `sanitizedSamples`), finite values are clamped to +-1e6. Fuzzed bit patterns and
  wild parameters yielded no NaN/Inf.
- Mono input: `DynamicsConfig.monoToStereo = true` duplicates to L/R (explicit route); otherwise the chain stays mono.
- Engine swap: `prepare()` builds a new engine off-thread and publishes it via an atomic pointer; `process()` adopts it at
  the next block boundary (pointer move only) and hands the old one back to `prepare()` for deletion. Delay lines of the
  new engine start empty, so a swap mid-stream causes a one-time discontinuity: do it between sessions.

## Telemetry (REQ-A10)
Raw input (pre-trim): `inputPeakDb`, `inputNearFullScale` (any sample >= -0.5 dBFS), `inputPossibleClipping` (run of >= 3
consecutive near-full-scale samples; the count is a design choice, not from the spec), `inputClipRunMax`.
Limiter-controlled output: `outputPeakDb` (sample peak), `outputTruePeakDb` (4x estimate), `outputOverFullScaleSamples`,
`limiterActive`, `limiterActiveSamples`, `limiterGrDb/MaxDb`, `compressorGrDb/MaxDb`. Hold values clear on `resetHolds()`.
`metadataJson()` places these in separate `rawInput` and `limiterControlledOutput` objects and states
`"trueTpLimiter":false`. Reads are relaxed-atomic snapshots; fields can be one block apart.

## True-peak (honest status)
`TruePeakMeter` is a 4x polyphase interpolation meter (48-tap Kaiser-windowed sinc designed in-house, not the normative
BS.1770 Annex 2 coefficients). Measured steady-state error on unit sines (0.02..0.45 fs, several phases): +0.0 dB over-read,
under-read bounded by the 4x grid (`20*log10(cos(pi*f/4))`, 0.17 dB at fs/4, 0.44 dB worst measured at 0.4 fs). It reads
about +0.9 dB high on an abrupt start (step overshoot). **A true-peak limiter is not implemented and not qualified.**
The limiter is sample-peak only; a test shows a -1 dBFS sample-peak ceiling lets an fs/4 tone at 45 degrees reconstruct to
about +2 dBTP (measured 2.1 dBTP). The spec's "-1 dBTP in qualified true-peak mode" and "within 0.2 dB with an independent meter" remain open (DEC for true-peak limiter).

## WAV writer (REQ-R05)
- Format: classic RIFF/WAVE, PCM, 24-bit, 1 or 2 channels, 44-byte header. Header sizes are placeholders (0xFFFFFFFF) until the
  first header patch (every 2 s of audio and on `flush()`), and exact after `stop()`/segment close.
- Quantisation: `q = floor(x * 2^23 + 0.5)`, clamped to [-2^23, 2^23-1] (clips counted), NaN/Inf -> 0, little-endian two's complement.
  Reader inverse is `q / 2^23`. Floats that are multiples of 2^-23 round-trip bit-exactly (tested). Dither is off; optional TPDF (+-1 LSB, seeded `ss::Rng`).
- Segmentation: `segFrames = min(round(segmentSeconds * rate), (min(maxSegmentBytes, 2^32-1) - 44) / frameBytes)`; defaults 900 s
  and 4 GiB - 1 MiB so the 32-bit RIFF limit is never reached. Segment `k` covers absolute sample indices
  `[start + k*segFrames, start + (k+1)*segFrames)`; the journal records these boundaries and session-relative ns. A stream ending
  exactly on a boundary does not open an empty segment. RF64 is not implemented.
- Gap policy: `write()` takes the absolute capture index of the first frame. A forward jump of <= 250 ms inserts that many
  zero frames and records a `GapEvent` (`SilenceInserted`, start index/ns, duration, segment, label). A longer jump, or a 4th gap within
  60 s (`maxGapsInWindow=3`, `gapWindowSeconds=60`, configurable), finalises the open segment and fails with
  `TimestampInvalid`; later writes return the same error. A backward jump (duplicate delivery) drops the duplicated frames and records `OverlapTrimmed`.
  Gap events are capped at 1024 stored (excess counted in `gapEventsDropped()`).
- Journal: `SegmentJournalEntry` per segment (Active/Completed/Failed, first/end sample, frames, flushedFrames, bytes, start/end ns, gaps,
  `headerFinalised`), refreshed at open, `flush()` and close; `onJournal`/`onGap` callbacks fire on the writer's thread.
- Recovery: `recoverWav`/`recoverWavFile` derive the data size from the file length (data chunk assumed last), cut a trailing partial frame, and rewrite both
  sizes. With a journal figure (`RecoveryHints.expectedFrames`, e.g. `flushedFrames`) the report states how many frames are missing; without one it says the
  unflushed tail is of unknown size. Audio is never invented. The staging buffer (`bufferFrames`, default 8192 frames) is the bound on loss besides OS caches.
- Failure: any sink error marks the segment Failed and returns `DiskFull` (no paths in text); completed segments are untouched.
- Names: `<UTCcompact>_<sessionId>_<showName>_<seg:04>.wav` with sanitised parts (`[A-Za-z0-9_-]`, 48 chars max).
- File I/O uses `std::ofstream`; no fsync is issued, so power-loss durability beyond `flush()` is OS dependent (not qualified).

## Sync model (REQ-R03)
- `ClockMap`: stores up to 256 `(samplePos, hostNs)` pairs (>= 500 ms apart), fits `host = a + b*(pos-pos0)` with a Theil-Sen slope (median of pairwise
  slopes between pairs >= 25% of the window apart) and median intercept. `valid` needs >= 16 pairs spanning >= 20 s. Reports ppm, jitter (MAD), predicts
  host time for a sample and the inverse. Measured: +150 ppm injected with +-0.5 ms timestamp jitter recovered within 10 ppm (tests assert <10, exact without jitter <0.5);
  +300 ppm measured and flagged.
- `DriftController`: |measured| <= 200 ppm -> gentle correction (slew <= 10 ppm per update, 2 ppm deadband), `inPerOut = 1 + applied*1e-6` for the resampler.
  Beyond 200 ppm -> `error=true`, state Error, the already applied correction is kept and nothing larger is applied; clears below 180 ppm. Updates are expected about 1 Hz.
- `DelayCompensation`: `lookahead + filter + codecPriming` samples; `audioPtsNs(outIndex)` subtracts the total, or delay video by `videoPtsOffsetNs()`. Do one, not both.
- `LinearResampler`: Q32 fixed-point phase, chunk-independent bit-exact output, allocation-free, one frame zero history at start. End-to-end simulation: +150 ppm device
  over 2 minutes, 18.2 ms uncorrected vs 4.4 ms residual after the controller (the estimate needs 20 s to become valid, then slews in).

## Limits and failure behaviour
Bounds checked before allocation: rate 8k..384k, channels 1..2, lookahead 1..20 ms, WAV buffer 64..1048576 frames, segment budget > header+frame.
Errors use `ss::Error` categories: `InvalidArgument`, `UnsupportedCapability`, `DiskFull`, `TimestampInvalid`, `DecodeFailure`, `PermissionDenied`.
No exceptions cross the API (except `std::bad_alloc` from allocation in `prepare`/writer construction paths, off the real-time thread).

## Tests (`tests/unit/dynamics`, 38 cases, a few seconds)
`test_dsp.cpp`: defaults/ranges, static curve (336 combinations, worst error 9e-5 dB), trim/makeup, limiter ceiling on 6 stimuli x 4 ceilings x 2 configs, startup, channel linking,
silence, fuzz, bypass continuity and bit-exact delayed dry path, parameter smoothing, REQ-A10 meters, delay equals measured impulse delay, mono routing and prepare
validation, staged engine swap, true-peak meter accuracy and the sample-peak-limiter caveat. `test_rt.cpp`: counting `operator new` (zero allocations in `process`, parameter
changes, bypass toggles, telemetry, engine adoption, resampler, clock map). `test_wav.cpp`: round trip, quantisation, dither, segmentation, budgets, header patching, recovery
(memory and disk), naming, gap/overlap policy, sink failure, journal callbacks. `test_sync.cpp`: drift recovery and flagging, outlier robustness, controller, resampler, end-to-end, delay record.

## Status
- Done: everything in the task list items 1-4, tested on Linux/gcc 13 with `-Wall -Wextra -Wshadow` (no warnings), 3 consecutive runs green, ASan/UBSan debug run.
- Partial: true-peak is a meter only; **true-peak limiter not implemented and not qualified** (section 12 enhanced profile open). The 4x meter filter is not the BS.1770 reference set.
- Not done / not qualified: no RF64; no fsync/durability qualification; no WASAPI/QPC packet source (the app feeds `ClockMap`); no flash-click A/V sync measurement or 1 h drift run (hardware);
  no CPU-budget measurement on the BASE-tier machine (design cost is roughly 100 multiply-adds per frame including the optional true-peak meter, turn it off with `truePeakMeter=false`);
  Windows/macOS not built here (portable C++17, CI covers them). Sync acceptance (80/40 ms, 20 ms/h) is therefore unverified; only the logic is.
- Tolerances (0.1 dB curve, 0.1 dB ceiling) are the spec's proposed values, met in these offline tests.

## Integration API
```
// pseudo-code (C++17: fill the config structs field by field; names shown for clarity)
// setup (control thread)
ss::dynamics::DynamicsProcessor dyn;
dyn.setParams(params);                              // defaults = section 12; returns ParamReport (what was clamped)
auto info = dyn.prepare({.sampleRateHz=48000, .inChannels=2, .monoToStereo=false}).value();   // PrepareInfo
// info.outChannels, info.processingDelaySamples (= lookahead frames)
auto dc = ss::dynamics::makeDelayCompensation(info, filterSamples, codecPrimingSamples);
// DSP worker thread, per AudioBlock from capture->DSP queue (raw capture is analysed separately, before dynamics)
dyn.process(block.interleaved, outBuf, block.frames);   // false until first prepare() adopted
// UI/diag (10 Hz): dyn.telemetry(); settings changes: dyn.setParams(...); dyn.resetHolds();
// recording metadata: dyn.metadataJson() + dc.toJson()
// disk thread
FileSinkFactory fac(dir, makeRecordingBaseName(clock.wallUnixNs(), sessionId, showName));
WavSegmentWriter wav({.sampleRateHz=48000, .channels=info.outChannels}, fac);
wav.onJournal = ...; wav.onGap = ...;               // forward to the session journal / Show Doctor
wav.start(firstSampleIndex);
st = wav.write(processedInterleaved, frames, block.firstSampleIndex /*absolute capture index*/, block.flags);
// st not ok => stop recording, surface st.error() (TimestampInvalid = gap policy, DiskFull = I/O); wav.stop() finalises
// crash recovery on next start: recoverWavFile(path, {true, journalFlushedFrames}) -> report.disclosure for the UI
// sync (capture thread feeds, media worker reads, ~1 Hz)
ClockMap map; map.addPacket(packetSamplePos, qpcNs); DriftController dc2; auto d = dc2.update(map.estimate());
// d.error => show "audio/video clock mismatch"; else LinearResampler rs; rs.setRatio(d.inPerOut) on the audio fed to the encoder.
// video PTS = host QPC; audio PTS = dc.audioPtsNs(outputSampleIndex) (or videoPtsOffsetNs on video, not both).
```
Needs from others: none. Notes for the integrator: the capture block's `kAudioDiscontinuity` flag should accompany the index jump so `WavSegmentWriter` counts it;
the DSP worker must feed `process()` the frames in capture order with no reordering; keep `prepare()` calls off the audio thread.
