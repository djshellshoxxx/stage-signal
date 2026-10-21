# Module Reference: dynamics

**Purpose.** The dynamics half of MOD-03 and the recording audio path. A compressor and limiter with lookahead, a true-peak meter, the sync model for recording (clock mapping and drift), and a WAV segment writer. Real-time safe: no allocation after `prepare()`.

**Depends on.** `base`.

## Public headers

| Header | Contents |
|---|---|
| `processor.hpp` | `DynamicsProcessor`, `DynamicsParams`, `DynamicsConfig`, `DynamicsTelemetry`, `ParamReport`, `ParamBit` |
| `truepeak.hpp` | `TruePeakMeter` (4x oversampled meter) |
| `sliding_extreme.hpp` | `SlidingExtreme` (lookahead maximum) |
| `sync.hpp` | `ClockMap`, `ClockMapConfig`, `ClockEstimate`, `DriftController`, `DriftConfig`, `DriftDecision`, `DriftState`, `GapEvent`, `GapKind`, `DelayCompensation` |
| `wav_writer.hpp` | `WavSegmentWriter`, `WavWriterConfig`, `IByteSink`, `FileSink`, `FileSinkFactory`, `MemorySink`, `MemorySinkFactory`, `SegmentInfo`, `SegmentState`, `SegmentJournalEntry`, `WavRecoveryReport` |
| `wav_reader.hpp` | `WavData`, `WavInfo` reader used by tests and recovery |

## Tests

`tests/unit/dynamics`: 38 doctest cases in `test_dsp.cpp`, `test_rt.cpp`, `test_sync.cpp` and `test_wav.cpp`, with `dsp_helpers.hpp`.

## Status (from `docs/specs/dynamics.md`)

- Done, tested on Linux (gcc 13), `-Wall -Wextra -Wshadow` clean, ASan and UBSan debug run.
- Partial: the true-peak meter is a meter only. **The true-peak limiter is not implemented and not qualified.** The 4x filter is not the BS.1770 reference set.
- Not done or not qualified: RF64 (the writer stays under the 4 GiB RIFF limit instead), fsync and durability, a WASAPI or QPC packet source (the app feeds `ClockMap`), a flash-and-click A/V measurement, a one-hour drift run, and a CPU budget on a BASE machine. Design cost is about 100 multiply-adds per frame. Turn the meter off with `truePeakMeter=false` if needed.
- Sync acceptance (80 ms and 40 ms, 20 ms per hour) is unverified. Only the logic is tested.
- Tolerances (0.1 dB curve, 0.1 dB ceiling) are the spec's proposed values.

## Limits

Sample rate 8 kHz to 384 kHz. Channels 1 or 2. Lookahead 1 to 20 ms. WAV buffer 64 to 1,048,576 frames. Segments: default 900 seconds, capped at 4 GiB minus 1 MiB so the 32-bit RIFF limit is never reached. The app spec asks for 1 hour or 2 GiB; the two are not reconciled yet. Errors: InvalidArgument, UnsupportedCapability, DiskFull, TimestampInvalid, DecodeFailure, PermissionDenied.
