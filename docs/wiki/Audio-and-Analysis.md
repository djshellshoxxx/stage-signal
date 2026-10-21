# Audio and Analysis

**Status: partly done.** The decision logic that uses audio features is in the core (`show`, `base`) and is tested with supplied feature values. The audio analyser that produces those values (tempo, key, band energy, onsets, Energy Arc detection) and the device capture layer are **not in the repository yet**. Treat tempo and key readings in this wiki as design targets, not as tested results on real music.

## What the app is meant to do

| Feature | What the operator sees | Status |
|---|---|---|
| Input level | Meter per channel, clip indicator, trim from -24 to +12 dB | Planned (app layer) |
| Band energy | Low (30 to 250 Hz), mid (250 Hz to 2 kHz), high (2 to 12 kHz) | Planned (analyser not in repo) |
| Onset | A flash-free "hit" event that drives scenes and effects | Logic in `base` type only; detector planned |
| Tempo | BPM with a confidence value and source (Estimated, Tap, Manual) | Planned; override logic done in `show` |
| Key | Key and mode with confidence | Planned; override logic done in `show` |
| Energy Arc | 0 to 1 energy, phase Steady, Build or Drop | Scene-choice and drop logic done in `show`; detector planned |

## Tempo

- Range accepted for manual BPM: 30 to 300. The operator spinner in the UI is specified as 40 to 240.
- **Tap tempo:** average of the last 8 taps. A gap longer than 2.5 s starts a new sequence. Two taps are needed.
- **Manual override** wins over the detector until released.
- StageSignal does not guess where bar 1 is. The bar anchor is set by the first tick or by the operator's "bar" action (`markDownbeat`). Beat and bar counts run from that anchor.

## Key

Key values run from 0 to 23: 0 to 11 are major keys and 12 to 23 are minor keys, with the tonic at value mod 12. A key override works the same way as tempo.

Key detection accuracy on real music is unproven. The labelled-corpus work is listed as open in [Known Limitations](Known-Limitations) (L-03).

## Energy Arc

The Energy Arc is a 0 to 1 level plus a phase (Steady, Build, Drop).

- Level maps to a scene tier: Low below 0.33, Medium below 0.66, High above.
- A Drop phase reserves Drop-tier scenes for a drop. The drop fires on the next bar line when tempo is known. Without tempo it fires unquantised, and the UI says so.
- A drop has a 4 second cooldown. A drop that is still waiting after 8 seconds is skipped.
- In silence or with unknown tempo the arc reports Steady and never triggers a drop.
- You can turn the Energy Arc off per show. Off means the show behaves as it did before the arc existed.

The acceptance target in the spec is that a drop lands within one beat on at least 90 percent of a synthetic build-then-drop set, with at most 10 percent false drops on steady material. That is a synthetic test target, not a claim about real tracks.

## Input setup

Use a clean feed from the desk or the DAW. Keep the meter below clipping on loud passages. The Show Doctor warns when input clipping was seen in the last minute. Sample rates 44.1 kHz and 48 kHz are the supported set. Other rates are greyed out.

## Plugin mode

In CLAP mode the host supplies the audio. StageSignal passes it through unchanged and does not add gain or dynamics on the main bus. Host timing is not available through the wrapper, so audio-to-video timing relies on calibration (see [Known Limitations](Known-Limitations), P-03).

## Dynamics (recording path)

The `dynamics` core has a compressor, a limiter with lookahead, a true-peak meter and a WAV segment writer. These apply to the recording tap only. The true-peak limiter is **not** implemented or qualified (see [Module Reference: dynamics](Module-Reference-dynamics)).
