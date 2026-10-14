# Lighting (MOD-10) notes

Code: `src/lighting/` (artnet, sacn, fixture, engine, usb_dmx, mock_sink). Tests: `tests/test_light_*.cpp`.

## Safety model
- Default **Disarmed**; nothing is transmitted while Disarmed. Arming needs an explicit single-use `ArmToken`
  (`requestArmToken` then `arm`). Faults, link loss, reconnect, patch changes and disarm all leave the engine Disarmed
  and invalidate outstanding tokens: it never re-arms by itself (REQ-L05).
- Unresolved patch conflicts (overlapping slots without one Owner and one Yielded fixture) block arming.
- Precedence per channel: emergency blackout > lighting blackout > Disable Strobing > manual override > programmed cue > audio > profile default.
  Blackout is fixture-aware: each channel goes to its profile `blackout` value, else its `safe` value (pan/tilt/macro safe values are kept; not all zeros).
- Preview/setup/benchmark use `LightingEngine::makeDryRun`, which owns a `DryRunSink` and cannot be given a real sink (REQ-L03).
- Graceful stop/disarm sends safe frames 3x per universe (sACN with the stream-terminated bit) before going silent.

## What this cannot guarantee
**The app cannot guarantee a physical blackout after cable, network, switch, node or power loss.** Receivers may hold the last
received levels. Before relying on blackout, configure and test the receiving side:
- Art-Net nodes: set the node's output-loss behaviour (hold vs. blackout vs. fixed scene) and its data-loss timeout.
- sACN receivers/gateways: set the network-data-loss behaviour; E1.31 specifies a 2.5 s network data loss timeout before a source is considered gone.
- Fixtures: set the "no DMX signal" mode to blackout where the fixture offers it.
Re-test after any firmware or configuration change; record the result as the node/fixture qualification.

## Numbering and transports
- Art-Net universe = 15-bit wire port-address (0..32767), shown as `Net:SubNet:Universe (port-address N)`. sACN universe = 1..63999.
  The two are never converted into each other. Unicast is the default; broadcast (including ArtPoll discovery) needs explicit opt-in.
- sACN: unicast or deliberate multicast 239.255.hi.lo, port 5568, priority default 100. Synchronization packets are not implemented.
- USB: no adapter is hardware-qualified yet (`UsbDmxAdapterRegistry::qualified()` is empty). The ENTTEC DMX USB Pro widget framing
  is implemented and golden-tested but labelled "not hardware-qualified". A USB serial port alone is never treated as compatibility.
