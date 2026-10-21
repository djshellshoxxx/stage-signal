# Lighting and DMX

**Status: packet and engine logic done and unit-tested; no real fixture, node or adapter has been tested.** The app's network and serial transport is planned.

> Read this first: StageSignal's lighting code is tested against hand-built packet goldens and an in-tree parser. Passing those tests proves the packets match the layout we wrote down. It does not prove that a real node or fixture will accept them. No lighting output should be trusted for a live show until you have run your own test with your hardware.

## Protocols

| Protocol | Transport | Notes |
|---|---|---|
| Art-Net | UDP port 6454 | ArtDmx, universes addressed by Port-Address (Net, Sub-Net, Universe). Zero-based on the wire. |
| sACN (E1.31) | UDP port 5568, multicast `239.255.<hi>.<lo>` | Universe 1 to 63999. Priority 0 to 200, default 100. |
| USB DMX (serial) | Native serial wrapper | Only adapters the owner has qualified are shown as supported. The registry currently marks every adapter as unqualified. |

Art-Net and sACN universe numbers are kept apart in the code. You cannot accidentally pass one where the other is expected.

## Arming

Lighting starts **disarmed**. Output only runs after an explicit Arm.

- Arm shows an inline confirmation: destination, interface, universe and fixture count. You press Arm to confirm.
- Dry run sends nothing. It shows what would be sent.
- After any fault, reconnect, interface change or destination change, Arm is required again.
- A show that loads with lighting armed shows a warning in the Show Doctor.

## Blackout

Blackout writes zeros to every patched channel and holds them. A lighting blackout latches and beats every cue by the next tick. Restore is explicit.

Limits:

- A blackout cannot switch off a fixture that ignores DMX, a dimmer pack with its own programme, or a projector's own feed (S-05).
- If the cable or power is lost, the last frame the fixture received is what it keeps. We have not tested node timeouts or safe-state settings (L-02, and see [Safety and Blackout](Safety-and-Blackout)).

## Fixture profiles and patches

- Profiles describe channels by function (intensity, colour, strobe, pan, tilt, and so on).
- A patch sets universe, start address and footprint. Start plus footprint must fit in 512 slots.
- Conflicting patches are reported and cannot be armed.
- Built-in profiles: generic dimmer, RGB, RGBW, and a five-channel dimmer, RGB and strobe profile.
- Strobe lock covers only channels declared with the Strobe function. A strobe mode hidden in a Macro range is not covered unless the profile marks that channel as Strobe.

## Rates and limits

| Item | Value |
|---|---|
| Frame rate | 25 Hz default, 40 Hz maximum |
| Universes | 4 |
| Slots per universe | 512 |
| Fixtures | 256 |
| Profiles | 64, each with up to 64 channels |
| Audio mappings | 128 |

## Testing status

| Area | Status |
|---|---|
| Art-Net and sACN builders and parsers | Done. Goldens and in-tree parser. |
| Fixture profiles, resolver, fades | Done. Unit tests. |
| Engine states (arm, dry run, blackout latch, fault, reconnect) | Done. Unit tests with a mock sink. |
| Real Art-Net or sACN node or receiver | Not tested |
| USB DMX adapters | Not tested. Frame builders exist. |
| sACN sync, discovery, priority arbitration, ArtSync, RDM | Not implemented |

Some primary specification PDFs were not read in the build session. The layouts were checked against secondary sources. A conformance pass is still needed before any protocol is called supported.

## Planned

- Real transports in the app (UDP sender bound to a chosen interface, serial wrapper).
- Setup for destination, universe, interface and rate on the MIDI/Lighting tab.
- Hardware qualification with named nodes and adapters. Each entry needs the model, driver, OS and result.
