# Scenes and Effects

**Status: core done, app wiring not built.** The scene renderer and both effects exist in the portable core (`render`) with golden-frame, property and allocation tests. Nothing in `main` draws them on screen yet.

## Scene catalogue

Twelve scenes ship in the default pack. Costs are relative to ColorField (1.0) on the development machine. They are an admission hint, not a guarantee. The real numbers for the 800 MHz budget tier need the benchmark tool, which is not built yet.

| Scene | Look | Audio use | Cost | Can flash |
|---|---|---|---|---|
| SpectrumBars | 32 gradient bars with caps | each bin drives one bar | 0.45 | no |
| MirrorBars | Bars mirrored left, right, up and down | low bins at the centre | 0.25 | no |
| WaveformRibbon | Three drifting waveform lines | waveform, onset brightens | 0.45 | no |
| GeometricOrbit | 5 to 7 bodies on elliptical orbits | low band pulses, high band sizes | 1.2 | no |
| RadialPulse | Expanding palette rings | low band gain, onset glow | 1.4 | no |
| ParticleField | 48 to 160 drifting glow particles | high band size, onset twinkle | 0.8 | no |
| NeonGrid | Scrolling perspective grid, sun at horizon | low band brightens grid | 2.0 | no |
| Tunnel | Checker tunnel with spiral twist | low band lightens dark checks | 2.0 | no |
| ColorField | Four-corner colour wash | low band value, onset lift | 1.0 | yes |
| Starburst | Rotating rays | low band width, onset core | 2.7 | yes |
| PlasmaLite | Sine plasma through a hue palette | low band value, onset shifts palette | 1.1 | no |
| LissajousScope | Lissajous curve with halo | waveform wobbles the radius | 1.2 | no |

Scenes with seeded shapes (orbit count, ray count, curve a and b) take the show seed, so a show looks the same on every machine. Idle scenes animate from elapsed time, not frame count.

## Strobe flag

Only ColorField and Starburst can produce a flash, and only on a real onset event. With **Disable strobing** on, they switch to a bounded fallback:

- ColorField caps its full-frame add at about 6 percent and drops the onset lift.
- Starburst removes the full-frame white add and only widens its rays slightly.

The other ten scenes render the same bytes whether strobing is on or off. See [Safety and Blackout](Safety-and-Blackout).

## Effects

### Prism Mirror

A kaleidoscope fold.

- Segment count: integer 2 to 16. Default 6.
- Amount 0 leaves the frame unchanged. Amount 1 is the full fold. Values in between blend linearly.
- The sampled wedge rotates at about one turn per 40 seconds at speed 1.
- Cost: about 2.5 times ColorField.
- Not affected by the strobe lock.

### Bloom Pulse

A glow that breathes with the low band and onsets.

- Downsample, bright pass, two box blurs at quarter resolution, additive composite. It never darkens the image.
- Amount 0 is off.
- With strobing disabled, the pulse is held in a narrow range (64 to 112), so it cannot flash.
- Cost: about 4.5 times ColorField at 320 x 240, and independent of content.
- With no audio events it holds a steady glow. It does not pulse by itself.

## Targets and limits

- Effects apply only to the layers and outputs they are assigned to. Other outputs are unchanged.
- Render buffers are limited to 4096 x 4096 and checked before allocation.
- After setup, the render path does not allocate.

## Planned

- Operator controls for Prism Mirror and Bloom Pulse on the Live tab (planned in `app-ui.md` 5.1).
- Per-output routing of scenes in the app.
- Benchmark numbers for the BASE tier. These are not measured yet.
