# Module Reference: base

**Purpose.** Shared contracts used by every other core module: the action registry, the feature snapshot from analysis, time, results and errors, lock-free queues and mailboxes, a deterministic random generator, and small utilities. It is header only. It has no I/O.

**Namespace.** `ss` (types also under `ss::base` names where listed).

## Public headers

| Header | Contents |
|---|---|
| `actions.hpp` | `ActionId` (stable numeric registry), `isSafetyCritical`, `ActionSource`, `TriggerKind`, `ShowMode`, `RouteKind`, `OutputKind`, `TriggerLevel` |
| `types.hpp` | `Ns` (int64 nanoseconds), `Revision`, `Generation`, `OutputId`, `FeatureSnapshot`, `TempoState`, `KeyState`, `ValueSource`, `Action`, `ActionResult`, `AudioBlock`, `AudioBlockView`, `PixelBuffer`, `ScheduledMidi`, `ShowSnapshotLite`, `CapabilityStatus` |
| `time.hpp` | `IClock`, `SystemClock`, `FakeClock`, `kNsPerSec`, `kNsPerMs`, `samplesToNs` |
| `error.hpp` | `ss::Error`, `ErrorCategory`, `Recoverability`, `Status`, `Result<T>` |
| `queue.hpp` | `SpscQueue<T>` (bounded; drops and counts when full, never blocks the producer), `LatestMailbox` |
| `rng.hpp` | `Rng` (seeded, same output on every platform) |
| `sha256.hpp` | `Sha256` |
| `utf8.hpp` | UTF-8 validation and code point counting |

## Key rules

- `ActionId` values are stored in show files. Append only, never renumber.
- Error categories: InvalidArgument, UnsupportedCapability, PermissionDenied, DeviceLost, TimestampInvalid, ResourceExhausted, DecodeFailure, EncoderUnavailable, DiskFull, TransportFailure, SchemaUnsupported, Cancelled, InternalFault.
- `FeatureSnapshot` carries band energy (low 30 to 250 Hz, mid 250 Hz to 2 kHz, high 2 to 12 kHz), onset flag, Energy Arc value and phase, input clipping flags, tempo, key, source time and a silence flag. It is produced by the `audio` analyser, which is not in the repo yet.

## Tests

`tests/unit/base`: 9 doctest cases, all passing in the local run. The base tests do not use `REQ-` names.

## Status

Done as shared contracts. No planned behaviour here waits on hardware.

## Limits

- `SpscQueue` is single producer, single consumer. Do not use it with several producers.
- `LatestMailbox` gives the newest value only. Earlier values are lost by design.

## Owner and dependents

Owner: the core. Every module depends on `base`. Change it only with an integrator's note, because a change here touches everything.
