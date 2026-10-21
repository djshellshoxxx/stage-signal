# Quick Tour

**Status: planned.** The layout below comes from `docs/specs/app-ui.md`. The current `main` build shows only a placeholder editor with the name on it. Use this page to learn the design, not the current build.

## Window

The operator window is designed for 800 x 600 logical pixels. A compact layout takes over below 800 x 600. The smallest supported size is 640 x 480.

| Area | Height | Contents |
|---|---|---|
| Status bar | 48 px | Read-only chips, left to right: Show, Audio, Mode, Tempo, Key, Camera, DMX, REC, LIVE. Enter on a chip jumps to its tab |
| Tab area | rest | Seven tabs, listed below |
| Bottom strip | 60 px | Visual, lighting and emergency blackout, Restore, Pause, Resume, Next, Brightness and Intensity (44 px buttons). Visible on every tab |

## Tabs

| Tab | Shortcut | Use it for |
|---|---|---|
| Live | Ctrl+1 | Scene pads (8, in banks), target selector, preview, tempo and key, mode, Energy Arc, looks, effects, undo and A/B snapshots |
| Setup | Ctrl+2 | Audio device and input route, displays and roles, outputs, safety, appearance, show files, benchmark, privacy |
| Media/Text | Ctrl+3 | Imported GIF, video and image files, and the text Draft, Preview, Publish panel |
| MIDI/Lighting | Ctrl+4 | MIDI ports, learn and mappings, clock out; Art-Net or sACN destination, universe, rate, arm |
| Record/Broadcast | Ctrl+5 | Recording folder and format, dynamics, stream endpoint and video settings |
| Diagnostics | Ctrl+6 | Show Doctor, thread heartbeats, journal viewer, support bundle, Show Report |
| Help | F1 or Ctrl+7 | Topic list and keyboard map |

## Live tab, in short

- **Target selector** (always visible): chooses which visual output the next action applies to.
- **Scene pads**: a 4 x 2 grid. Pads 1 to 8 select scenes on the current bank. States are Idle, Active, Queued, Unavailable. Strobe scenes carry a FLASH badge and lock when strobing is disabled.
- **Tempo** and **Key**: tap tempo (key T), a BPM spinner, a key picker. "Auto" releases a manual key.
- **Looks**: Speed, Hue and Saturation.
- **Effects**: Prism Mirror and Bloom Pulse, each with an amount and an on/off state (see [Scenes and Effects](Scenes-and-Effects)).
- **Snapshots**: Undo, Redo, and A/B store and recall with a fade time.

## Feedback you should expect

- Every state has a text or icon label, not colour alone.
- Disabled controls show the reason as a tooltip.
- Long operations (import, device scan, benchmark) show Busy. The UI does not freeze.

## Known gaps

The UI is specified but not built. Expect the final layout to differ in small ways. Colour values are proposals until they are checked against the Circuit Drift Labs design system.
