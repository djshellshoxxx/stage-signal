# Lighting (MOD-10)

Namespace `ss::lighting`, code in `core/lighting`, tests in `tests/unit/lighting` (target `test_lighting`).
Depends on `base` and `model` only. No sockets, no serial ports, no threads, no globals.

## Scope and requirements

| REQ | Where it is implemented | Main tests |
|---|---|---|
| REQ-L01 profiles map universe/start/channels to intensity/colour, bounds and conflict tests | `fixture.*`, `resolver.*` | `REQ-L01: ...` in `test_fixture.cpp` |
| REQ-L02 Art-Net and sACN transmit validated frames to the selected destination, receiver and packet tests | `artnet.*`, `sacn.*`, `engine.*` | goldens in `test_packets.cpp`, frame tests in `test_engine.cpp` (a parser acts as the mock receiver) |
| REQ-L03 preview/setup/benchmark transmit zero datagrams | `engine.cpp` routing, `sink.hpp` MockSink | `REQ-L03: ...` (real sink has attempt/datagram/byte counters, all asserted 0) |
| REQ-L04 master blackout beats every cue by the next tick, safe values retained | `resolver.cpp`, `engine.cpp` latch | `REQ-L04: ...` |
| REQ-L05 disconnect/fault reported, reconnect stays disarmed until explicit Arm | `engine.cpp` | `REQ-L05: ...` |
| REQ-L06 named USB adapters with protocol/version/qualification | `usb_dmx.*` | `REQ-L06: ...` |
| REQ-S12 (lighting side) strobe lock on controlled fixture channels | `resolver.cpp` | `REQ-S12: ...` |

Section 14 items covered: fixture profiles as data, start + footprint <= 512, conflicts warn and cannot arm, resolve
precedence, audio mappings from `FeatureSnapshot` at the lighting rate (25 Hz default, 40 Hz max), fades in the physical
channel domain by function, fixture-aware blackout, Art-Net/sACN numbering kept apart, termination on graceful stop,
remain disarmed after reconnect, named USB adapters. Section 8: lighting is one latest-state frame per universe, the
blackout priority latch bypasses the ordinary path.

## Protocol facts and how they were verified

Primary sources are the Art-Net specification (art-net.org.uk PDF, SRC-14) and ANSI E1.31-2018. **The primary PDFs could
not be fetched in this session** (the Art-Net PDF request was blocked), and ANSI E1.31 is not freely downloadable. Packet
layouts therefore rest on the sources below plus the published layouts as I know them; golden tests prove the builders
match those layouts, not that a real node accepts them.

Verified against a fetched secondary source this session:
- Art-Net (Wikipedia): UDP port 6454 (0x1936); packet starts `Art-Net\0`; ArtDmx OpCode 0x5000 sent little-endian;
  protocol version 14; sequence 1..255 with 0 disabling it; data length even, 2..512; universe sent little-endian.
- sACN (ESPHome `e131_packet.cpp` struct, go-sacn constants citing E1.31-2018 Appendix A): ACN identifier
  `ASC-E1.17\0\0\0`, root vector 4, framing vector 2, DMP vector 2, field order and byte offsets (property values at 125,
  full packet 638 bytes), multicast group `239.255.<universe hi>.<universe lo>`, start code 0 required, sync packet
  vectors.
- Enttec DMX USB Pro (API document hosted at git.dgnum.eu/HackENS/pyjecteur, "Enttec DMX USB Pro API"): start 0x7E,
  label, length LSB, length MSB, data, end 0xE7; label 6 "Output Only Send DMX Packet Request"; payload = start code +
  channels; documented payload size 25..513 (hence zero padding up to 24 slots); other labels listed.

From my knowledge of the published specs, NOT confirmed from a fetched source (treat as unverified until the PDFs are
checked): ArtDmx field order after the version (Sequence, Physical, SubUni, Net, LengthHi, Length) and the SubUni/Net split
of the 15-bit Port-Address; ArtPoll layout (14 bytes: header, version, TalkToMe, DiagPriority) and the ArtPollReply
offsets used by `parsePollReply` (239 byte reply, minimum accepted 207); E1.31 flags+length encoding (0x7 nibble +
12 bit length measured from the length field), option bits (Preview 0x80, Stream_Terminated 0x40, Force_Synchronization
0x20), DMP header bytes `0xA1`, `0x0000`, `0x0001`, three terminated packets on stream end; Open DMX timing (250 kbaud
8N2, BREAK/MAB with margin over the DMX512-A minimums).

Not implemented: ArtSync, ArtAddress, RDM, sACN synchronization packets, universe discovery packets, sACN priority
arbitration beyond the static priority field.

## Public API

Headers in `core/lighting/include/ss/lighting/`:

- `limits.hpp` constants. `addressing.hpp`: `ArtNetPortAddress` (wire 0..32767, zero-based) and `SacnUniverse` (1..63999,
  one-based) are distinct, non-convertible types; `artnetFromUserOneBased`, `artnetFromNetSubUni`, `artnetLabel`,
  `sacnLabel`, `parseIpv4`, `Endpoint`, `sacnMulticastEndpoint`.
- `artnet.hpp`: `buildDmx` (even length 2..512), `parseDmx`, `nextSequence` (1..255, skips 0), `buildPoll`,
  `parsePollReply` (untrusted input: bounds checked, strings sanitised).
- `sacn.hpp`: `buildData` (1..512 slots, universe 1..63999, priority 0..200, CID non-zero, name truncated to 63 bytes on a
  UTF-8 boundary, preview/terminated options), `parseData`.
- `usb_dmx.hpp`: `adapterRegistry()`, `findAdapter`, `isGenericCompatible` (always false), `buildEnttecProDmx`,
  `buildOpenDmxFrame`. Data only; the app opens the serial port.
- `fixture.hpp`: `builtinProfiles()` (generic dimmer, RGB, RGBW and a 5 channel dimmer+RGB+strobe), `validateProfile`,
  `validatePatches` (-> `PatchReport`), `physicalFromNormalized`. Profiles/patches are `model::FixtureProfile` /
  `model::FixturePatch`.
- `resolver.hpp`: `FixtureResolver::configure/resolve/safeFrames/sourceOf`, `FixtureLook`, `AudioMapping`, `ResolveInputs`.
- `sink.hpp`: `IDatagramSink`, `Datagram`, `MockSink` (counters + fault injection).
- `engine.hpp`: `LightingEngine`, `LightingConfig`, `configFromSettings`, `validateConfig`, `makeArtPollDatagram`.

## Data flow

```
show state ──► LightingInputs{purpose, generation, ResolveInputs{blackout flags, strobe lock, manual, cue, audio, fade}}
                      │ tick(now) at 25 Hz (max 40)
              LightingEngine ── FixtureResolver ──► per-universe 512 slot frames (always returned, latest state)
                      │ route: Armed && Live ? real IDatagramSink : internal MockSink
                      ▼ rate gate (period = 1 s / rateHz) with priority bypass for blackout / strobe-lock edges
              Art-Net ArtDmx or sACN data datagrams (full 512 slots per universe)
```

Resolve order per channel: emergency blackout > lighting blackout > manual override > cue > audio > profile default.
The strobe lock (`strobingDisabled`) sits between the layers and the blackout: every Strobe function channel is forced to
its profile `safeValue` whatever layer asked for strobe. Fades are linear slews in the channel's physical domain (8 or 16
bit) for Intensity/colour/Pan/Tilt; `fadeNs` is the time for a full-scale traverse. Strobe, Macro and Other snap and are
never interpolated. Blackout and the strobe lock are never faded; the underlying look keeps tracking, so restore returns
the current look.

Blackout values are fixture-aware: Intensity/colour/Strobe channels go to the channel `blackoutValue`; Pan/Tilt/Macro/Other
hold `safeValue`. 16-bit channels apply the 8-bit configured value to the coarse byte with a zero fine byte.

Audio mapping (`AudioMapping`): source band low/mid/high, level (-60..0 dBFS -> 0..1), onset, energy arc -> target function
of one fixture or all; one-pole smoothing with `alpha = dt / (tau + dt)` (no libm, identical across platforms). Audio is the
lowest layer: it only supplies functions that no cue or manual look sets.

Patch rules: a conflict is an overlap in one universe between patches that are not a declared mirror (same non-zero
`group`, same profile, same start). Overlaps are `Warning` severity with `blocksArm = true`; the operator resolves ownership
by disabling one patch (`PatchOptions::disabledPatchIds`) or declaring a mirror group. Any invalid profile in the library
also blocks arming (conservative). Invalid patches are excluded from output; more than 4 universes (provisional cap) or 256
fixtures are errors.

## Engine state machine

`Disarmed` (default) -> `Armed` only through `arm()`; `DryRun` through `startDryRun()`. `arm()` requires: configured, patch
without blocking issues, valid destination (Art-Net unicast or explicit broadcast; sACN unicast or explicit multicast),
a sink, and `sink.connected()`. Arming generates a new arm token that is stamped on every real datagram.

- Isolation (REQ-L03): the real sink is touched only when `state == Armed && purpose == Live`. Disarmed, DryRun, Preview,
  Setup and Benchmark route to the engine's own `MockSink`. Real and mock routes have separate rate gates.
- Blackout latch (REQ-L04): `latchBlackout(bool emergency)` / `restoreBlackout()` are atomics usable from any thread (MIDI,
  UI). The next `tick` resolves blackout, ignores the rate gate (`priorityBypass`) and marks datagrams `priority`. The latch
  persists until `restoreBlackout()`; the show snapshot flags OR into it. Strobe-lock assertion also bypasses the gate.
- Faults (REQ-L05): a send error, `connected() == false` on a tick, or `reportTransportLost()` produce a `TransportFault`
  event and state `Disarmed` with `needsExplicitArm()`. `transportReconnected(now, newGeneration)` never re-arms.
- Graceful stop (`disarm`/`stop` while Armed): 3 safe (blackout-equivalent) frames per universe, then for sACN 3
  Stream_Terminated packets carrying safe data, then `Disarmed`. Art-Net has no termination. If the link is down this is
  best effort; nodes and fixtures may hold their last values (cannot be guaranteed; see Status).
- Stale generations: `LightingInputs.generation` != engine generation (and non-zero) discards the inputs, reuses the last
  good look, and still honours blackout and strobe-lock requests (only the safer direction is taken from stale data).
  `MockSink::setExpectedGeneration` shows the same rule for sinks.
- Events (`drainEvents`) are bounded to 64; `stats()` exposes counters.

## Limits and failure behaviour

512 slots per universe; 4 universes; 256 fixtures; 64 profiles, 64 channels per profile; 128 audio mappings; rate 1..40 Hz
(41+ rejected); sACN priority 0..200; bounds are checked before allocation. Errors use `ss::Error` categories, never
exceptions, no secrets or paths in text.

## Tests

`test_packets.cpp` (goldens, boundaries, numbering, USB, registry), `test_fixture.cpp` (profiles, bounds, conflicts,
precedence, safe values, strobe lock, fades, audio, limits), `test_engine.cpp` (isolation, arming, blackout latch, faults,
reconnect, graceful stop, rate limiting, stale generations, determinism). All deterministic, no sleeps, no sockets.

## Status

Done (portable logic, unit-tested): Art-Net ArtDmx build/parse, ArtPoll build and ArtPollReply parse, sACN data/termination
build/parse, multicast mapping, numbering helpers, fixture profiles/validation/resolver, fades, audio mapping, strobe lock,
LightingEngine (arm/dry-run/blackout latch/fault/reconnect/stop/rate gate/generations), USB frame builders and registry.

Not done / partial, honestly:
- **No real node, fixture, receiver or adapter was ever tested.** Packets are checked against hand-built goldens and an
  in-tree parser only; this proves packet behaviour, not fixture safety (spec section 16). Primary Art-Net/E1.31 documents
  were not read (see "verified" list above); a conformance pass against the pinned PDFs and a real receiver (e.g. a
  second-party sACN/Art-Net monitor) is still required before any tier is claimed.
- USB adapters: registry entries are all `unqualified: no hardware test`. The engine does not drive USB; `LightingConfig`
  has no USB transport (model `LightingProtocol` has only ArtNet/Sacn). The app layer must pull `TickResult.frames` itself
  and apply the same Armed/Live gate. A model change (protocol enum) and an app serial sink are needed.
- Blackout on cable/power loss cannot be guaranteed; node timeout and safe-state configuration are not tested.
- No sACN sync, no universe discovery, no priority arbitration, no Art-Net sequence reordering test with a real node.
- Moving-head semantics (fine channels beyond a coarse/fine pair, ranges, macro maps) are generic; qualification per fixture
  is future work. The strobe lock covers channels declared with the Strobe function; a fixture exposing strobe through a
  Macro/Other range is not covered unless its profile marks that channel as Strobe.
- The show module must translate cue `lightingRefs` into `FixtureLook`s; the group/look model is not defined in `model`.
- Unpatched slots are sent as 0 (frames are always 512 slots).

## Integration API

```cpp
#include "ss/lighting/engine.hpp"
using namespace ss::lighting;

// 1. Own one engine on the lighting worker thread; real sink = JUCE UDP adapter implementing IDatagramSink.
class UdpSink : public IDatagramSink {            // app layer: bind to the selected NIC, non-blocking
  Status send(const Datagram& d) override;        // d.dst, d.bytes, d.generation, d.armToken, d.priority
  bool connected() const override;
};
LightingEngine engine(&udpSink, /*generation*/ 1);

// 2. Configure from the show document (also on show load). Fails on bad rate/destination text.
auto cfg = configFromSettings(show.lighting, cidPersistedPerInstall, "StageSignal");   // sACN CID: make once, store locally
engine.configure(show.fixtureProfiles + builtinProfiles(), show.patches, cfg.value(), PatchOptions{}, audioMappings);
engine.patchReport();                              // show warnings/errors; blocksArm issues disable the Arm button

// 3. Operator actions (ActionId::ArmLighting / DisarmLighting / LightingDryRun)
engine.arm();            // Status; refused with a reason while conflicts or no transport
engine.startDryRun();    // mock only
engine.disarm(now);      // safe frames + sACN termination

// 4. Blackout actions (ActionId::LightingBlackout / EmergencyBlackout / RestoreBlackout), any thread, lock free
engine.latchBlackout(/*emergency=*/true);  engine.restoreBlackout();

// 5. Every lighting tick (25 Hz default, 40 max; clock = IClock monotonic ns)
LightingInputs in;
in.purpose = Purpose::Live;                 // Preview/Setup/Benchmark ALWAYS stay on the internal mock sink
in.generation = udpGeneration;              // 0 if unstamped
in.resolve.strobingDisabled = snap.strobingDisabled;   // REQ-S12
in.resolve.lightingBlackout = snap.lightingBlackout;
in.resolve.manual = manualLooks;  in.resolve.cue = cueLooks;  in.resolve.audio = latestFeatureSnapshot;
const TickResult& r = engine.tick(clock.nowMonotonic(), in);   // r.frames = latest per-universe state for the UI preview

// 6. Transport events
engine.reportTransportLost(now, "interface down");
engine.transportReconnected(now, newGeneration);   // stays Disarmed; UI must show "Disarmed, press Arm"
// 7. Status bar / diagnostics
engine.state(); engine.needsExplicitArm(); engine.stats(); engine.drainEvents();
// Art-Net discovery (explicit opt-in only): makeArtPollDatagram(cfg) -> send via the sink yourself, parse replies with artnet::parsePollReply.
// USB-DMX: usb::adapterRegistry() lists named adapters (all unqualified); build serial bytes with usb::buildEnttecProDmx(frame.slots...).
```

Needs from others: `show` maps cue `lightingRefs` and manual overrides to `FixtureLook` and feeds `snap.strobingDisabled` /
`lightingBlackout`; `doctor` may read `engine.patchReport()`, `state()`, `stats()`; a possible `model::LightingProtocol`
extension if USB transports are to be selectable.
