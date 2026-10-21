# Media and Text

**Status: compose core done; media import and decode planned.** The compositor can layer a camera feed, a text overlay, and a GIF or video frame chosen by the caller. The media module (import, decode, flash-risk checks) and the app's file picker are not in the repository yet.

## Media

Media stored in a show bundle uses these extensions: PNG, JPEG, BMP, GIF, MP4 and MKV. Bundles store media by content hash, so the same file used twice is stored once.

| Setting | Range |
|---|---|
| GIF speed | 0.25 to 4 times |
| Video speed | 0.25 to 2 times |
| Loop | on or off |
| Crop | per item |

Import runs in a background worker. A file that cannot be read shows "Could not read this file. Original scene kept." Imported media carry a visible warning until a flash check has run.

**Flash risk is a heuristic.** A video that the check marks as clean can still flash. Treat imported video the same as any other strobing source (see [Safety and Blackout](Safety-and-Blackout)).

## Camera

The compositor has a camera layer with these behaviours:

- Placement: full frame, picture-in-picture, or split (side by side or stacked).
- When the feed is lost, the layer is either hidden or replaced by a placeholder, set by policy.
- Colour treatments.
- Split-screen layouts.
- The camera must not publish to an audience output unless you choose it. The Show Doctor warns when it is selected but unavailable, or published by accident.

Camera capture itself is an app-layer job. JUCE has no Linux camera implementation, so Linux camera support is missing (P-04). Camera behaviour is tested with fake frames only (L-04).

## Text

Text is handled as a draft that you publish:

1. **Draft** the text in the text panel.
2. **Preview** it on the operator side. Preview never reaches an audience output.
3. **Publish** sends it to the named target outputs.

Rules:

- Text must be valid UTF-8 and at most 4096 code points.
- Only Publish changes audience outputs.
- Unpublished drafts show a Show Doctor warning.
- Text is drawn with a built-in 5 x 7 font in the core. Font support beyond that is not built.

## Planned

- Media import and decode with a background worker.
- Flash-risk analysis of imported media.
- Media list with thumbnails, status (Ok, Missing, Invalid) and loop, speed and crop controls.
- Camera capture on Windows and macOS.
