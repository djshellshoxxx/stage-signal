# Show Report

**Status: core done (`diag`, unit-tested on Linux); the export dialog is planned.** The report builder produces the HTML and CSV from a journal. The dialog that picks categories and saves the files is not built.

## What it contains

Inputs: the event journal (scene changes, markers, device changes, recording and broadcast events, gaps, errors) and feature samples taken about once a second (time, BPM, key, energy arc, arc phase, scene).

Sections:

- **Setlist:** one section per marker, from that marker to the next (or to the end of the session). Each shows median BPM, most common key and peak energy. With no markers, one "Session" section.
- **Drops:** each point where the energy arc entered the Drop phase.
- **Scene usage:** activations, time on each scene, share of the session.
- **Operations:** device, adaptation, recording, broadcast and error events.
- **Discontinuities:** journal gaps and any stretch longer than 5 seconds with no feature samples.

## Files

| File | Format | Notes |
|---|---|---|
| Report | Single HTML file | Inline CSS (light, dark and print), inline SVG chart of energy (solid) and BPM (dashed), marker lines, drop dots. No scripts, no external resources. |
| Events | UTF-8 CSV | Quoted cells. Cells that start with `=`, `+`, `-`, `@`, tab or CR are neutralised, so spreadsheets do not run them as formulas. |
| Setlist | UTF-8 CSV | One row per setlist section. |

## Rules

- The same journal always gives byte-identical output, whatever the input order.
- No secrets, no absolute file paths, no raw audio or video appear in the output. The test scans for them.
- An empty journal writes nothing and shows "Nothing to report yet."
- A disk-full or read-only folder gives an error and leaves no partial file.
- Bounds: 20,000 events and 100,000 samples are read at most, and tables are capped at 500 rows with a "N more rows" note.

## Limits

- The HTML has not been viewed in Windows, macOS or Linux browsers, so its layout and dark mode are not reviewed yet.
- Printing is unchecked.
- Key names use sharps only (C#, not Db).
- The journal does not detect wall-clock jumps. Both clocks are recorded so a reader can see the difference.
