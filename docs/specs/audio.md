# MOD-03 Audio analysis (`ss::audio`)

Module: `core/audio` (static library `ss_audio`, deps: `base`). Tests: `tests/unit/audio` (`test_audio`).
Requirements: REQ-A03, A04, A05, A06, A07, A08, A10 (analysis side); spec sections 5, 8, 12 "Analysis design", 18
(T-DSP, T-TEMPO, T-KEY, T-RT). Also owns the **Energy Arc** feature (DEC-13).

## 1. Scope

In: RMS/peak meters, 3 band energies, spectrum and waveform for visuals, onset detection, tempo estimation, tap/manual
tempo control, key estimation, manual key control, input clipping indication, Energy Arc (long-term energy plus
steady/build/drop phase).
Out: dynamics/limiter and the recording audio bus (module `dynamics`), device capture (app layer), persistence of the
manual overrides (module `persist`/`show` own save/load; the controllers here only hold the value), BPM/key visibility
per output (REQ-A09, router/render).

## 2. Public API (headers in `ss/audio/`)

| Header | Types |
|---|---|
| `analyzer.hpp` | `Analyzer`, `AnalyzerConstants`, `OnsetEvent`, `AnalyzerStats` |
| `tempo.hpp` | `TempoEstimator`, `TempoLimits`, `TempoController`, `TempoControllerConfig` |
| `key.hpp` | `KeyEstimator`, `KeyLimits`, `KeyDiagnostics`, `KeyController` |
| `energy_arc.hpp` | `EnergyArc`, `EnergyArcParams`, `EnergyPhase` |
| `fft.hpp` | `Fft` (own scalar radix-2) |

`Analyzer::prepare(rateHz, channels)` -> `Status`. Rate 8000..192000 (44100/48000 are the qualified ones), channels 1
or 2; otherwise `InvalidArgument`/`UnsupportedCapability`. All buffers are allocated here (and in `reset()` nothing is
allocated). `process(const AudioBlockView&)`, `snapshot()`, `peek()`, `copySpectrum(float*32)`, `copyWaveform(float*256)`,
`drainOnsets(OnsetEvent*, cap)`, `stats()`, `setEnergyArcParams()`.

## 3. Data flow and DSP definitions

```
AudioBlockView -> sanitise -> meters (5 ms sub-windows)  --------------------------> rmsDb/peakDb/clip/silent
                           -> ring(1024) --hop 256--> Hann 1024 -> FFT (stereo packed in one complex FFT)
                                 per-channel power, averaged over channels ("energies summed per channel"):
                                 bands, spectrum(32), positive flux -> onsets -> TempoEstimator, EnergyArc
                           -> (L+R)/2 mono -> KeyEstimator (own decimation to ~4 kHz)
```

* **Sanitising.** NaN/Inf become 0 (`stats.nonFiniteSamples`); |x| > 64 is clamped to +-64 (`stats.clampedSamples`).
  Blocks with another rate/channel count than `prepare()` are ignored (`stats.mismatchedBlocks`). `kAudioSilent` or a
  null buffer are treated as zeros. Any block size from 1 sample up is supported; results are independent of block size.
* **Discontinuity** (`kAudioDiscontinuity`): internal clock advances by `firstSampleIndex - expected` (capped to 1 h),
  the FFT window is zero-flushed and the next flux frame is suppressed so the splice cannot create a false onset; tempo,
  key and arc history are kept.
* **Meters.** Rolling 50 ms window = 10 sub-windows of 5 ms (updated every 5 ms). `rmsDb = 20 log10(rms)`,
  `peakDb = 20 log10(peak)`, floor -120 dB. Mono: channel 1 mirrors channel 0. `silent` = max channel RMS < -70 dBFS.
* **Clipping (REQ-A10).** A frame is near full scale when any channel has |x| >= 0.944061 (-0.5 dBFS).
  `inputNearFullScale` = peak over the 50 ms window >= that; `clipRunLength` = longest run of consecutive near-full-scale
  frames inside the window. This looks at the *input* samples only, so a downstream limiter can never hide it; the
  processed/output meters belong to module `dynamics`. UI text must say "possible input clipping".
* **Bands (REQ-A03).** 1024-point Hann window (periodic), DC bin excluded, bins whose centre lies in
  [30,250), [250,2000), [2000,12000) Hz, upper edge bounded by Nyquist. `band[i]` = 2 x mean-square power of the
  signal inside the band, so a sine of amplitude A inside a band reads A^2 (full scale = 1.0, "0..~1"). Hann leakage:
  tones within ~2 FFT bins (about 95 Hz at 48 kHz) of a band edge split their energy between two bands.
  Updated every 256 samples.
* **Spectrum.** 32 bins, edges `30 * (16000/30)^(i/32)` Hz. Value = `clamp((10 log10 P + 70)/70, 0, 1)`, P = total FFT
  power in the bin in band units (bins narrower than one FFT bin interpolate). Instant attack, release `0.92` per hop
  (time constant ~64 ms at 48 kHz). Bins at or above Nyquist are 0.
* **Waveform.** Newest 1024 mono-mixed samples, groups of 4 averaged, 256 points, oldest first, clamped to [-1, 1].
* **Thread model.** One analysis thread owns an `Analyzer` (prepare/process/snapshot/copy*). Publish snapshots to other
  threads through a mailbox. `process()` takes no locks and allocates nothing (T-RT test with a counting global
  `operator new`). One 4096-point key FFT runs every ~0.5 s inside `process()` (sub-millisecond); the other work is
  amortised (<= one 1024 FFT per 256 samples).

### 3.1 Onsets (REQ-A04)
Per hop: `L_k = log(1 + 1000 * sqrt(P_k))` for FFT bins up to min(12 kHz, Nyquist). Flux = sum of positive `L_k - L_k(prev)`
averaged inside 24 log-spaced bin groups, then averaged over the groups (so kick-only onsets are not diluted by many
high-frequency bins). Adaptive threshold = max(0.06, mean + 3.5 std of the last 64 hops excluding the two newest).
Peak picking: previous frame is a local maximum above the threshold. Refractory >= 100 ms. After an onset the threshold
floor is `0.6 * onsetFlux * exp(-dt/0.25 s)` so decay/pitch-glide bumps of one hit do not re-trigger.
`onsetStrength = 1 - exp(-flux/0.5)`. Latency from the physical onset: about 1 hop (peak picking) + 5..20 ms; the
reported event time is the end of the peak frame minus 256 samples. `snapshot()` returns `onsetFired = true` once per
detected onset (latched until the next `snapshot()`), `drainOnsets()` returns the timestamped event queue (128 deep,
oldest overwritten and counted in `stats.onsetsDropped`).

### 3.2 Tempo (REQ-A05, REQ-A06)
`TempoEstimator` consumes the per-hop flux. Envelope = flux minus a 0.4 s running mean, half-wave rectified, 5-tap Hann
smoothed (removes hop-alignment dependence). Normalised autocorrelation `r(lag)` (mean removed, unbiased) over the last 8 s,
lags for 40..240 BPM, computed incrementally (a few lags per hop, bounded work per call), a new measurement every 0.5 s.
First estimate needs >= 5 s of envelope. Pick: strongest local maximum of `r * prior` (mild log-normal prior around
120 BPM, sigma 1.5 octaves), then **octave resolution**: if a peak at lag/4, /3 or /2 (largest k first, within +-3%) reaches
>= 80% of the main peak and is <= 200 BPM, take it (prefers the pulse rate over its sub-harmonics; accepts that
swing/weak off-beats stay at the beat). Parabolic interpolation gives sub-frame resolution.
`confidence = clamp((r - 0.20)/0.50, 0, 1)` (heuristic monotone score, NOT a probability). Measurements with confidence
< 0.30, an envelope peak < 0.05 (no onsets worth a tempo; also stops float-noise periodicity of steady tones) or tempo outside
38..250 BPM are rejected.
Smoothing: first estimate needs two agreeing measurements (within 3%); then EMA 0.25 on BPM and 0.3 on confidence;
a different tempo needs 3 consecutive agreeing measurements (confidence is multiplied by 0.9 meanwhile).
`ageNs` = time since the last accepted update. Silence for 1.5 s or no accepted measurement for 8 s clears the estimate:
`TempoState{bpm=0, source=Unknown}` (never a numeric-zero valid tempo).
`TempoController` (REQ-A06): `tap(now)`, `setManual(bpm, now)`, `release()`, `resolve(estimate, now)`.
Tap: mean interval of the last <= 8 taps (`(last-first)/(n-1)`), needs >= 2 taps, a gap > 2.5 s starts a new sequence
(the previous override stays until the new sequence yields a tempo), result must be inside 30..300 BPM. Manual accepts
30..300 (NaN/Inf/out of range -> `InvalidArgument`, state unchanged). The latest user action wins (tap over manual and
vice versa). Overrides report confidence 1 and age since the action. `release()` returns to the estimate. No clock inside:
callers pass `Ns`, so tap behaviour is bit-deterministic.

### 3.3 Key (REQ-A07, REQ-A08)
`KeyEstimator` input: mono mix. Two cascaded Butterworth low-pass biquads (0.425 x decimated rate) then integer decimation to
~4 kHz (12 at 48 kHz, 11 at 44.1 kHz). Frames: 4096 points Hann, hop 2048 (0.51 s). Per frame: spectral peaks 60..1600 Hz
above -26 dB of the frame maximum, parabolic interpolation, weight sqrt(power), accumulated in a 36-bin (1/3 semitone)
pitch-class histogram, normalised per frame. Frames with decimated RMS below -60 dBFS are inactive. Context = ring of
24 frames (~12.3 s, spec 8..20 s); at least 16 active frames (~8.2 s) are required before any claim.
Evaluation every 4 frames (~2 s): tuning offset (-1/0/+1 third-semitone) with the most energy on pitch-class centres, fold to
12 classes, Pearson correlation against 24 rotated templates. **Profile used: Temperley (2005) major
`5.0 2.0 3.5 2.0 4.5 4.0 2.0 4.5 2.0 3.5 1.5 4.0`, minor `5.0 2.0 3.5 4.5 2.0 4.0 2.0 4.5 3.5 2.0 1.5 4.0`**
(tonic first). It was chosen over Krumhansl-Kessler because on triad-based synthetic material KK left only a 0.03
correlation margin between a major key and its relative minor (confidence suppressed for 6 of 12 major keys), Temperley
gives 0.17..0.24.
Confidence = `fit * margin`, `fit = clamp((r1 - 0.70)/0.20, 0, 1)`, `margin = clamp((r1 - r2 - 0.02)/0.08, 0, 1)`
(r1 best, r2 second best template). **Reporting threshold `KeyLimits::kMinConfidence = 0.50`**: a candidate below it is
suppressed even if a best template exists. State machine: no key -> needs the same candidate in 2 consecutive
evaluations; same key confirmed -> confidence EMA 0.4 and `ageNs` resets; different candidate -> old confidence x0.6 per
evaluation (gradual clearing) and a switch only after 2 consecutive agreeing evaluations; no usable candidate or too few
active frames -> x0.6 / x0.5 per evaluation until below threshold, then Unknown. About 12 s of silence empties the ring (24 frames).
`KeyController`: `setManual(tonic 0..11, minor, now)` (otherwise `InvalidArgument`), `release()`, `resolve()`;
confidence 1, source Manual, independent of audio. Save/load of the override is the persist/show module's job.
Not claimed: chord recognition, modulation tracking inside the window, key from drums (single notes, drum loops, white/pink
noise and random notes are tested to give Unknown).

### 3.4 Energy Arc (new feature, DEC-13)
Purpose: tell the show engine how intense the music currently is over the long term (`energyArc`, tier choice) and whether
a **build** (sustained rise) or a **drop** (sudden big return after a dip or build) is happening (`arcPhase`: 0 steady,
1 build, 2 drop), e.g. for bar-quantised drop hits.
Input: per hop, total power = sum of the three band values. Level `x = clamp((dB - floor)/(ceil - floor), 0, 1)`
with `dB = 10 log10(power)`, floor -50 dB, ceil 0 dB. Silence forces `x = 0`. Control rate 20 Hz. Trackers (one-pole):
short (0.25 s, fixed), **fast** (`fastTauS` 1 s) and **slow** (`arcTauS` 8 s, ~24 s to settle). `energyArc` = slow tracker.
Tunables (`EnergyArcParams`, validated; an out-of-range set returns `InvalidArgument` and changes nothing):

| Field | Default | Valid range | Meaning |
|---|---|---|---|
| `levelFloorDb` / `levelCeilDb` | -50 / 0 | [-90,-20] / [-30,6], span >= 20 dB | power mapped to 0 / 1 |
| `fastTauS` | 1.0 | [0.3, 3] | fast tracker |
| `arcTauS` | 8.0 | [4, 30] | slow tracker = published arc |
| `buildRise` | 0.08 | [0.03, 0.4] | fast-tracker rise over `buildWindowS` to enter Build (0.08 ~ 4 dB with the default map) |
| `buildWindowS` | 6.0 | [3, 12] | |
| `dropJump` | 0.20 | [0.08, 0.6] | short-tracker rise above its 4 s minimum (0.20 ~ 10 dB) |
| `dropDipDepth` | 0.10 | [0.03, 0.4] | depth of the dip before a drop |
| `dropHoldS` | 4.0 | [1, 10] | Drop phase length |
| `refractoryS` | 8.0 | [2, 30] | minimum time between drops |

Rules (all deterministic; thresholds are the defaults above):
* **Build** enters when the fast tracker rose >= `buildRise` over the last `buildWindowS` **and** >= 0.02 over the last 2 s.
  It leaves (hysteresis) after at least 2 s when the 3 s slope falls below a quarter of the entry rate
  (`buildRise/buildWindowS`), i.e. on a plateau or a fall.
* **Drop** fires when (a) the short tracker is >= `dropJump` above its minimum of the last 4 s and at least half of that
  within the last 0.5 s, **and** (b) either Build has lasted >= 2 s, or the fast-tracker peak that precedes the 4 s minimum
  (12 s look-back) exceeds that minimum by >= `dropDipDepth`; and the refractory time has passed; needs 4 s of history.
  It holds `dropHoldS` and then returns to Steady (or Build if a new rise has started).
* Not drops: music starting from silence (nothing precedes the minimum), steps down, +-4 dB beat fluctuation, a plain
  verse-to-chorus step without dip or build (deliberately conservative).

## 4. Limits and failure behaviour

| Item | Behaviour |
|---|---|
| Invalid `prepare` args | `Status` error, analyzer stays unprepared; `process()`/`snapshot()` on an unprepared analyzer are safe no-ops (snapshot silent, tempo/key Unknown) |
| Rate/channel mismatch block | ignored and counted |
| NaN/Inf/huge samples | sanitised and counted, all outputs stay finite and in range (fuzz test) |
| Onset queue full | oldest dropped, `stats.onsetsDropped` |
| Memory | fixed after `prepare()`: well under 1 MB (tempo 8 s window, key frame buffers, FFT tables) |
| Time base | `sourceNs` = capture time of the end of the last block when `kAudioTimestampValid` was set, else sample-clock time; ages are measured in processed audio time |

Determinism: no randomness; identical input and block pattern give identical output on one build. Bit-exactness across
compilers/libm is not claimed (logs/exps are used), tests use tolerances.

## 5. Tests (`tests/unit/audio`, doctest, ~5 s CPU)

* `test_dsp.cpp` (T-DSP): FFT vs reference DFT; calibrated tones (100/130/700/1000/5000/9000 Hz, 44.1/48 kHz, mono/stereo)
  select the right band and RMS/peak within 0.5 dB; Nyquist-bounded bands at 8 kHz; anti-phase stereo; block-size
  independence (1,7,64,480,1023,4096); REQ-A10 near-full-scale threshold and run length; silence; synthetic pulse corpus
  (8 seeded 10 s clips, 44.1/48 kHz, four timbres, -70/-50 dB noise bed): >= 95% matched within +-50 ms and <= 5% false;
  refractory; latch; discontinuity; spectrum/waveform.
* `test_tempo_key.cpp` (T-TEMPO, T-KEY): click corpus 60..180 BPM within 2% after 10 s (48 kHz mono, 44.1 kHz stereo);
  varied-timbre pulses; swing/weak off-beats; silence/noise/steady tone -> Unknown; expiry; tempo change; tap/manual
  determinism and error cases; key: all 24 keys on synthetic I-IV-V-I progressions (additive synthesis), major scale,
  detuning +-40 cents, silence/noise/drums/single note/random notes -> Unknown, evidence delay, gradual change, manual override.
* `test_arc.cpp`: build/drop/hysteresis/refractory, false-positive guards, determinism, parameter validation, end-to-end.
* `test_rt.cpp` (T-RT): zero heap allocations in `process`/`snapshot`/`copy*`/`drainOnsets` (replaced global `operator new`),
  fuzz with NaN/Inf/huge values/random flags/mismatched formats/null buffers/zero-frame blocks, recovery after fuzz.

## 6. Status

* **Done**: everything listed above is implemented and covered by the tests; builds clean with `-Wall -Wextra -Wshadow` (gcc),
  run under ASan+UBSan locally.
* **Accuracy on real music is NOT measured.** Tempo and key were calibrated only on synthetic material (click trains,
  synthetic hits, additive-synthesis triads/scales). Tempo thresholds (confidence mapping, 0.80 octave rule, 200 BPM
  preference), the Temperley profile choice, key fit/margin ramps, the 0.50 reporting threshold and the Energy Arc
  thresholds are provisional and must be re-tuned on the frozen licensed corpus (DEC-10) with separate calibration and
  test sets. Expect half/double-tempo errors on real music (the octave rule prefers the faster related pulse up to
  200 BPM), relative-major/minor confusions, and no key on heavily percussive or atonal material. Energy Arc has only
  been exercised with synthetic level curves and tones; musical validation on real tracks is outstanding.
* The processed/output clipping meter (second half of REQ-A10) belongs to module `dynamics`; this module provides the
  input side only.
* No hardware or device qualification was performed (no capture device code in this module). CPU cost was only measured
  on a shared development VM (about 100x real time for mono 48 kHz), not on the BASE tier; T-PERF/T-RT worst-callback
  timings on target hardware are outstanding.
* Key estimation downmixes to (L+R)/2 before decimation; fully anti-phase stereo would not yield a key (band/onset analysis
  does use per-channel energies).

## 7. Integration API (for the app integrator)

```cpp
#include "ss/audio/analyzer.hpp"
ss::audio::Analyzer an;                                   // owned by the analysis/dynamics worker thread
auto st = an.prepare(negotiatedRateHz, channels);         // off the real-time path; channels 1 or 2 (route the 2 chosen ones)
// worker loop, for every captured block (IF-01), in order, any frame count:
an.process(blockView);                                    // RT-safe: no alloc, no locks. Set kAudioDiscontinuity after overflow.
// at the UI/show rate (e.g. 30-60 Hz), same thread:
ss::FeatureSnapshot fs = an.snapshot();                   // consumes the onset latch; use peek() to look without consuming
float spectrum[32]; an.copySpectrum(spectrum);            // 0..1, visuals
float wave[256];    an.copyWaveform(wave);                // -1..1, visuals
ss::audio::OnsetEvent ev[32]; size_t n = an.drainOnsets(ev, 32);   // timestamped onsets for beat-aligned effects
// publish fs / spectrum / wave to other threads through a mailbox (Analyzer itself is single-thread).
```
* Overrides live in the **show** module's state; it owns one `TempoController` and one `KeyController`:
  `TempoState t = tempoCtl.resolve(fs.tempo, nowNs)`, `KeyState k = keyCtl.resolve(fs.key, nowNs)` where `nowNs` is any
  monotonic clock (use the same one for `tap()`); persist `overrideSource()` plus the value yourself. `tapCtl.tap(nowNs)`
  from the Tap action, `setManual()`/`release()` from the BPM/key actions. Tap has no internal timer: call `resolve()` each
  tick.
* UI wording: `tempo.valid()` false -> "Unknown" (never show 0 BPM); show `confidence` and `ageNs`; `inputNearFullScale`
  -> "possible input clipping" (separate from the output meter of `dynamics`).
* Energy Arc: `fs.energyArc` (0..1) and `fs.arcPhase` (`ss::audio::EnergyPhase`: 0 Steady, 1 Build, 2 Drop). Drop is
  held for 4 s: quantise the drop hit to the next bar on the transition 0/1 -> 2. Tunables: `an.setEnergyArcParams(p)`
  (returns `Status`; invalid sets are rejected and change nothing). Defaults suit line-level or room-mic input after
  the capture gain; adjust `levelFloorDb` for very quiet inputs.
* Re-`prepare()` on every device/rate/channel change (new `Generation`); call `reset()` to clear state without reallocating.
* Diagnostics for Show Doctor/Report: `an.stats()` (non-finite, clamped, mismatched, discontinuity, onset drop counters).
* Needs from others: none. The show module needs the `TempoController`/`KeyController` wiring above; no change to `base`.
