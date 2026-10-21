# Recording and Streaming

**Status: planned (app layer and `record` and `broadcast` modules are not in the repository).** The `dynamics` core has the WAV segment writer and the sync model that the recording path will use. Everything else on this page is specified, not built.

## Recording

What gets recorded:

- **Program audio:** the processed bus, after the dynamics stage.
- **Program video:** the chosen output, if FFmpeg is available.

Formats:

| Format | When | Notes |
|---|---|---|
| WAV, 32-bit float or 24-bit PCM | Audio | Header rewritten every 5 seconds, so a crash loses at most 5 seconds. The app spec asks for segments of 1 hour or 2 GiB. The dynamics core currently defaults to 15 minute segments, capped near 4 GiB. |
| Matroska (`.mkv`) | Video | Default. Playable up to the last flushed cluster after a crash. |
| Fragmented MP4 | Video | Alternative. Plain MP4 loses the whole file if it is cut off before the end. |

Segment names: `<show>_<UTC timestamp>_<n>.<ext>`. A segment is written with a `.part` suffix and renamed when closed.

Rules:

- Free space is checked at start and every 5 seconds. Under 2 GiB warns. Under 256 MiB stops with a clear message.
- The app does not report "Recording" until the first block is written.
- Recovery on next launch lists unfinished `.part` files with Keep, Repair (WAV header) and Delete. Nothing is deleted without a confirmation.
- Recording can never be started by a key press. Use the explicit button.

## Dynamics on the recording path

Recording has its own dynamics stage with a compressor, a limiter (with lookahead), input trim, and a bypass. The true-peak limiter is not implemented and not qualified, so do not rely on it to catch inter-sample peaks.

## Streaming

| Setting | Options |
|---|---|
| Endpoint scheme | RTMPS (default), RTMP (unencrypted, labelled), SRT, RTP on LAN |
| Stream key | Stored in the system keychain. Shown as "saved" only. Never shown again, never written to a log or a show file. |
| Video | Resolution, frame rate, bitrate, keyframe interval |
| Audio | Sample rate, bitrate |
| Reconnect | On or off. A countdown shows the next try (8 seconds in the spec's state table). Stop cancels a pending reconnect within 1 second. |

Broadcast uses FFmpeg as a separate process. FFmpeg is never bundled with StageSignal. If it is missing, broadcast shows a clear message and stays off.

Known limits (see [Known Limitations](Known-Limitations)):

- Stream keys reach the FFmpeg command line in version 1, which other local users can see in a process list (P-10).
- Broadcast is tested against a local loopback receiver only, not against live services (L-07).
- WebRTC and RTP contribution are deferred.

## Stop and shutdown

- Stop recording: the app closes the file cleanly. It waits up to 5 seconds for FFmpeg to finish the file, then terminates it.
- Stop broadcast returns control within 1 second.
- Closing the app stops both in order, with timeouts.

## Practical advice

- Test a short recording before every show, then play it back.
- Use a local disk. Network folders are allowed but write slowly and show a warning.
- Keep at least 2 GiB free.
- Sync between picture and sound on recordings is not measured yet (L-05). Do a flash-and-click test if sync matters for your edit.
