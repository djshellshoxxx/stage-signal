# Musical features: phrase tracking, key colour, beat-synced looks (FEAT-001..003)

Three pure, deterministic, portable C++17 units. No JUCE, no OS headers, no threads, no allocation in any call. Integer or fixed-point math wherever output must match across platforms.
Requirement-style IDs are `FEAT-xxx`; test names are `FEAT-xxx: ...` doctest cases. Traceability rows are in
`docs/TRACEABILITY.md` ("Added features").

Dependencies (per `docs/ARCHITECTURE.md`): `FEAT-001` in `audio` (base only); `FEAT-002` in `palette` (base only);
`FEAT-003` in `show` (base, model). No edits outside the new files, the
`docs/TRACEABILITY.md` rows and this file.

---

## FEAT-001 PhraseTracker (`ss/audio/phrase_tracker.hpp`, module `audio`)

**Scope.** Tracks the bar position inside a 4, 8 or 16 bar phrase from the bar counter of the beat grid and the Energy
Arc phase. Flags phrase starts and "pre-drop" (last bar of a phrase while the arc is in Build) so that scene changes can
quantise to phrase boundaries. Out: beat/bar detection itself (show `BeatGrid`, IF-03), persistence of the nudge (show).

**API.**
- `Status setParams(const PhraseParams&)`: `phraseBars` in {4, 8, 16}; `minConfidence` [0, 1]; `minVotes` [1, 64];
  `lockShare` [0.5, 1]. Invalid => `InvalidArgument`, previous params kept.
- `void onBar(int64 barIndex, EnergyPhase arcPhase, float confidence)`: once per bar boundary. `barIndex` comes from
  `BeatGrid::barAt()`. Repeated or older indexes are ignored.
- `void markPhraseStart(int64 barIndex)`: operator tap; manual lock that survives later votes until `reset()`.
- `void nudge(int bars)`: cumulative, wraps modulo 16. `nudgeBars()` reads it back.
- `PhraseState state()`: `locked`, `barInPhrase` (-1 unlocked), `phraseStart`, `preDrop`, `barsToPhraseStart`
  (1..phraseBars, 0 unlocked), `nextPhraseStartBar` (-1 unlocked), `lockShare`, `manual`.

**Algorithm.** Each Drop entry (phase != Drop on the previous bar, == Drop now) while the confidence gate is open votes
for `barIndex mod 16`. Votes are folded modulo the phrase length; the best residue wins and locks the phrase when it has
at least `minVotes` votes and share >= `lockShare`. Then `barInPhrase = (barIndex - start - nudge) mod L`,
`preDrop = (barInPhrase == L-1 && arc == Build)`.

**Limits.**
- Confidence gate closed (or NaN): `locked=false`, no flags, no votes. The lock itself is kept and resumes without drift
  when the gate reopens.
- A Drop is attributed to the first bar boundary at which it is observed. Detection latency larger than one bar shifts
  the phrase by one bar; use `nudge()` to correct it.
- 16-bar phrases with drops on an 8-bar grid split the votes 50/50 and do not lock; this is intended.
- Votes are unbounded `uint32` counters (no decay); `reset()` clears them.

**Tests** (`tests/unit/audio/test_phrase.cpp`): parameter validation; lock after enough votes and exact bar position,
boundaries and countdown; pre-drop only on the last Build bar; 16 and 4 bar phrases; confidence gating with no drift;
NaN gate; inconsistent drops do not lock; nudge and wrap; manual tap; duplicate bars and reproducibility.

**Status.** Done and tested. Not verified on real music: the drop detector's latency is the Energy Arc's, measured
only on synthetic arc sequences.

**Integration API.**
```
PhraseTracker pt;  pt.setParams(p);                       // phraseBars 8 by default
// once per bar boundary, on the show/analysis thread:
pt.onBar(grid.barAt(now), static_cast<EnergyPhase>(snap.arcPhase), snap.tempo.confidence);
PhraseState s = pt.state();
if (s.phraseStart)  ...                                    // align a phrase-quantised scene change
Ns when = grid.boundaryAfter(now, true);             // time of the next bar line; use for TriggerKind::NextBar
// s.nextPhraseStartBar: absolute bar of the next phrase start; s.barsToPhraseStart: bars until it (see note)
pt.nudge(+1);  pt.markPhraseStart(grid.barAt(now));       // operator controls
```
Note: `barsToPhraseStart` counts bar boundaries, so 1 means the next bar line starts a phrase.
Persist `nudgeBars()` with the show if it should survive a restart (show owns persistence).

---

## FEAT-002 KeyColorMapper (`ss/palette/key_color.hpp`, module `palette`)

**Scope.** Maps a detected key (`KeyState`: tonic 0..11, minor, confidence, source) to an output colour on the circle
of fifths. Major keys are light, minor keys darker and shifted toward blue. Low confidence holds the last stable colour,
then falls back to neutral. Output is smoothed so it never flickers and never breaches the luminance bounds. Out: lighting
or render application of the colour (the consumer), the palette-to-action wiring.

**API.**
- `Status setParams(const KeyColorParams&)`: `enterPermille` (confidence to start a candidate), `holdPermille` <= enter,
  `confirmNs` [1, 30] s, `staleNs` [2, 120] s, `maxSlewPerSec` [1, 255], `majorLightPermille` [300, 700],
  `minorLightPermille` [150, 500] and < major, `saturationPermille`, `lumMinPermille` [0, 500],
  `lumMaxPermille` [lumMin + 50, 1000].
- `void update(const KeyState&, Ns nowNs)`: call at >= 10 Hz with monotonic time.
- `Rgb8 color()`: slewed output. `Rgb8 target()`: where it is heading. `neutral()`, `stableTonic()`, `stableMinor()`.
- Pure helpers: `fifthsPosition`, `hueDeg`, `colorFor`, `neutralColor`, `relativeLuminancePermille`, `clampLuminance`.

**Algorithm.** Hue = `fifthsPosition(tonic) * 30` deg, where `fifthsPosition = (7 * tonic) mod 12`. Minor keys move up to
20 deg toward 240 deg. HSL(hue, saturation, lightness) is computed in integer permille, then each channel is clamped so
that WCAG relative luminance Y (Rec.709 weights on linearised sRGB, integer table) lies in
[lumMin, lumMax]. A candidate key must stay at or above `enterPermille` for `confirmNs` before it becomes the stable key.
Readings at or above `holdPermille` keep the stable key alive. After `staleNs` without such a reading the target is neutral.
The output moves toward the target by at most `maxSlewPerSec` per 8-bit channel per second and is clamped again after
each step.

**Limits.** Integer-only: identical output on all platforms. The luminance bounds are an engineering choice that keeps
colours legible and non-blinding; they are not a WCAG conformance claim for the whole show. Flicker is prevented by the
confirmation timer plus the slew rate; a key that alternates faster than `confirmNs` never changes the target. Slew of
the full colour range (about 140 codes) takes roughly 3.5 s at the default rate.

**Tests** (`tests/unit/palette/test_key_color.cpp`): parameter validation; circle-of-fifths adjacency and anchors; minor
darker and cooler for all tonics; golden C-major value; luminance bounds for extreme parameters; confirmation timing;
low-confidence hold then neutral; unknown key goes neutral; alternating keys never change the target; slew rate and
convergence; reset.

**Status.** Done and tested. Colour appearance on real projectors and the chosen luminance bounds are not qualified
(needs a human check on the rig).

**Integration API.**
```
KeyColorMapper km;  km.setParams(KeyColorParams{});
// every key update or at >= 10 Hz, with the same monotonic clock as the show:
km.update(snapshot.key, nowNs);
Rgb8 c = km.color();                 // 8-bit sRGB, apply to the colour channel of the output
// KeyColorMapper::neutralColor(p) is the colour to use when no key is ever known.
```
Map a manual key override (`KeyController::resolve`) into `KeyState` before calling; Manual sources are trusted with
confidence as provided.

---

## FEAT-003 BeatLooks (`ss/show/beat_looks.hpp`, module `show`)

**Scope.** Tempo-synced modulation values in 0..1 for intensity and colour steps: a sine, triangle, ramp or step pattern
per beat division (half, quarter, eighth, sixteenth; optional triplet; swing). Locked to the show `BeatGrid`. The
value is a pure function of `(grid, now, params)`, so lighting and render can each call `sample()` and see the same
value. Out: applying the value to fixtures or pixels (consumers), tap tempo (audio/show).

**API.**
- `Status setParams(const BeatLooksParams&)`: `division`, `triplet`, `shape` (Sine, Triangle, RampUp, RampDown, Pulse,
  Steps), `swing` [0, 0.5], `duty` [0.05, 0.95] (Pulse), `steps[16]` + `stepCount` [1, 16] (Steps), `strobingDisabled`.
- `BeatLooksSample sample(const BeatGrid&, Ns now) const`: `value` 0..1, `cycle` (absolute cycle index, use for colour
  steps), `valid`, `flashType`, `capped`, effective `shape`, `division`, `triplet`, `cyclesPerSec`.
- `static int cyclesPer12Beats(BeatDivision, bool triplet)`.

**Algorithm.** Rate in cycles per beat = `cyclesPer12Beats / 12` (half 1/2, quarter 1, eighth 2, sixteenth 4, triplet
x 3/2). Beat fraction and beat count come from `BeatGrid::boundaryAfter` / `beatAt`. Swing moves the boundary of the
second cycle of each pair to `0.5 + swing/2`. Sine uses a 256-entry raised-cosine table with linear interpolation.

**Limits.**
- Flash ceiling: Pulse and Steps never exceed 3 cycles per second (`BeatLooksLimits::kMaxFlashHz`). If the rate is too
  high the triplet is dropped first, then the division is halved step by step, and above 360 bpm the shape falls back to
  Sine. `capped` reports each reduction.
- `strobingDisabled` turns Pulse and Steps into Sine (`capped = true`).
- Grid not anchored or no tempo: `valid = false`, `value = 0`. Callers must not treat zero as a valid level.
- Smooth shapes (Sine, Triangle, Ramps) have no rate cap; they do not flash.

**Tests** (`tests/unit/show/test_beat_looks.cpp`): parameter validation; invalid grid; sine timing at quarter notes;
exact 4-beat periodicity for every shape, division and triplet; triplet and eighth rates; swing boundary; flash cap
for 16ths at 120 bpm; a 10 s sweep over 9 tempi, 8 division/triplet combinations and both flash shapes with no more than
30 rising edges; fallback above 360 bpm; strobing disabled; step pattern; range and repeatability.

**Status.** Done and tested on a synthetic beat grid. The 3 flashes/s limit is enforced in the output values. Whether a
given fixture or projector flashes less than that is a hardware question not answered here.

**Integration API.**
```
BeatLooks bl;  bl.setParams(params);                    // from the show's look settings
BeatLooksSample s = bl.sample(showBeatGrid, nowNs);     // same call from lighting and render
if (s.valid) intensity = s.value;                       // 0..1
int colourStep = static_cast<int>(s.cycle % paletteSteps);
// Set strobingDisabled from ShowSnapshotLite::strobingDisabled on every change.
```

---

## Status summary

| Feature | Module | Status |
|---|---|---|
| FEAT-001 PhraseTracker | audio | Done, tested (synthetic arc sequences only) |
| FEAT-002 KeyColorMapper | palette | Done, tested; luminance bounds need a rig check |
| FEAT-003 BeatLooks | show | Done, tested on a synthetic grid |

Nothing here is wired into the app layer; the app integrator uses the Integration API sections above.
