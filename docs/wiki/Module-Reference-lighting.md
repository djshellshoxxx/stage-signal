# Module Reference: lighting

**Purpose.** Lighting (MOD-10). Builds Art-Net and sACN packets, resolves fixture profiles and patches into per-universe frames, runs fades and audio mappings, and runs the lighting engine state machine (disarmed, armed, dry run, blackout, faulted). It transmits through an interface (`IDatagramSink`). The real sink belongs to the app. Tests use `MockSink`.

**Depends on.** `base`, `model`.

## Public headers

| Header | Contents |
|---|---|
| `limits.hpp` | Universe, slot, fixture, profile, mapping and rate limits |
| `addressing.hpp` | `ArtNetPortAddress` (zero-based on the wire), `SacnUniverse` (1 to 63999), conversions, `Endpoint`, `Ipv4`, `sacnMulticastEndpoint` |
| `artnet.hpp` | `buildDmx`, `parseDmx`, `nextSequence`, `buildPoll`, `parsePollReply` (untrusted input) |
| `sacn.hpp` | `buildData`, `parseData` (priority 0 to 200, CID, name up to 63 bytes) |
| `usb_dmx.hpp` | `adapterRegistry`, `findAdapter`, `buildEnttecProDmx`, `buildOpenDmxFrame`. Data only. The app opens the serial port. |
| `fixture.hpp` | `builtinProfiles`, `validateProfile`, `validatePatches` (-> `PatchReport`), `physicalFromNormalized` |
| `resolver.hpp` | `FixtureResolver` (configure, resolve, safe frames, source of each channel), `FixtureLook`, `AudioMapping`, `ResolveInputs` |
| `sink.hpp` | `IDatagramSink`, `Datagram`, `MockSink` (counters and fault injection) |
| `engine.hpp` | `LightingEngine`, `LightingConfig`, `TickResult`, `configFromSettings`, `validateConfig`, `makeArtPollDatagram` |

## Engine behaviour

- Starts disarmed. `arm()` needs a valid config, a connected sink and a patch without conflicts.
- `startDryRun()` is mock only. It sends nothing to the network.
- A blackout latch beats every ordinary frame by the next tick. Restore is explicit.
- A transport loss sets a fault. Reconnect stays disarmed until `arm()` is called again.
- `stop()` sends termination and disarms.
- Each tick returns one frame per universe.

## Tests

`tests/unit/lighting`: 60 doctest cases across `test_engine.cpp`, `test_fixture.cpp` and `test_packets.cpp`. Many names start with `REQ-L`. Packet goldens are in `test_packets.cpp`.

## Status (from `docs/specs/lighting.md`)

- Done, portable logic, unit tested: Art-Net build and parse, ArtPoll, sACN build and parse, multicast mapping, numbering, profiles, validation, resolver, fades, audio mapping, strobe lock, the engine state machine, USB frame builders and the registry.
- **No real node, fixture, receiver or adapter has been tested.** Packets match hand-built goldens and an in-tree parser only. That proves packet behaviour, not fixture safety.
- Some primary specification PDFs could not be read in the session. Field layouts that came from memory are marked unverified in the spec.
- USB adapters: every registry entry is marked unqualified. The engine does not drive USB. The model has no USB protocol value. The app must pull `TickResult.frames` and apply the same gate.
- Blackout on cable or power loss is not guaranteed. Node timeouts and safe states are not tested.
- No sACN sync, no discovery, no priority arbitration beyond the static field, and no Art-Net sequence reordering test with a real node.
- Moving-head fine channels and macro maps are generic, not per fixture.

## Limits

| Item | Value |
|---|---|
| Slots per universe | 512 (frames are always 512 slots, unpatched as 0) |
| Universes | 4 |
| Fixtures | 256 |
| Profiles | 64, each with up to 64 channels |
| Audio mappings | 128 |
| Rate | 1 to 40 Hz (41 and above rejected), default 25 |

All limits are checked before allocation. Errors use `ss::Error` categories. No exceptions.
