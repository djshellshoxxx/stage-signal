# Displays and Outputs

**Status: planned (app layer).** The render core can draw any scene at any size and fit it to a rectangle, rotate it, and map normalised geometry to pixels. Display enumeration, audience windows and hot-plug are specified in `docs/specs/app-adapters.md` section 4 and `app-ui.md` section 5.2, but not built.

## Concepts

| Term | Meaning |
|---|---|
| Output | A logical picture the show drives, for example "Main" or "Side". Each output has a route and a target. |
| Role | What a physical display is used for: Controls, Audience, Duplicate of another output, Independent, or Unused. |
| Target | The output that an action applies to. The Live tab always shows the target selector. |
| Controls window | The operator window. It must never be on the audience display. |

## Roles

- **Controls:** the operator window. The Show Doctor warns if an audience route points here.
- **Audience:** a full-screen output window on a chosen display.
- **Duplicate of X:** mirrors output X. A duplicate follows its source's scenes. Actions aimed at a duplicate go to its source.
- **Independent:** a separate root with its own scene state, changed only by actions aimed at it.
- **Unused:** the display is ignored.

Roles are saved by display identity. JUCE does not give a stable identity for every monitor, so two identical monitors can be ambiguous. Ambiguous displays stay unassigned until you confirm them.

## Identify and test pattern

- **Identify** puts a large number on each display for 5 seconds. Use it to match a window to a screen.
- **Test pattern** shows a border and grid, which helps with a VGA or projector check. A full VGA qualification has not been done (see [Known Limitations](Known-Limitations)).

## Audience window rules

- Audience windows show only the program picture. No operator panels, no diagnostics, no stream keys, no file paths.
- The diagnostics overlay can be enabled per output, with an extra confirmation.
- The BPM and key overlay and the text overlay can be toggled per output.
- Blackout hides the picture on that output.

## Preview

The Live tab preview runs at 5 to 15 frames per second and says "Preview". It does not transmit and is never the program output.

## Hot-plug

Displays can be added or removed while the show runs. A removed audience display stops that output and reports it. It does not stop audio, lighting or recording. A returning display is not switched back on until you confirm it.

## Platform limits

- Display identity is not stable in JUCE. The app adds platform code for it, but identical monitors may still need manual assignment (P-01).
- Audience windows opened from inside a CLAP host may land behind the host or on the wrong display. Test with your host (D-04 and `research-deferred.md` section 9).
- Linux has no screen reader support in JUCE (P-05).

## Output rendering

The render core works in software (CPU) only. GPU output is not part of the baseline. A frame is produced at the output's own resolution and rotation. Rotation supports 90, 180 and 270 degrees and portrait layouts.
