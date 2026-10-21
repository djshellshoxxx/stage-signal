# Show Doctor and Troubleshooting

**Status: Show Doctor planned.** The `doctor` module is specified (NEW-06) but not in the repository. The checks and their rules are fixed in `docs/specs/app-ui.md` section 10. The troubleshooting steps below are for the beta builds and the parts that exist today.

## Show Doctor

The Show Doctor is a read-only preflight. It runs on show load and before Arm, Start recording and Start broadcast. A Blocker stops that action and shows the reason.

It never sends a datagram, never opens a recorder or broadcaster, and never starts the camera. It only enumerates devices.

Findings have four levels: **Blocker**, **Warning**, **Info** and **Pass**. The banner reads "Ready", "Ready with N warnings" or "Not ready: N blocker(s)".

| Id | Checks | Blocker when | Warning when |
|---|---|---|---|
| D-AUD-1 | Audio device present and running | none open | silent for 5 s or more |
| D-AUD-2 | Sample rate and route | n/a | unsupported rate |
| D-AUD-3 | Input clipping in the last minute | n/a | clipping seen |
| D-DSP-1 | Audience route matches a display | n/a | route unmatched or only one display |
| D-DSP-2 | No audience route on the controls display | any | n/a |
| D-DSP-3 | Identical monitors | n/a | ambiguous |
| D-SAF-1 | Strobe lock vs flashing items | n/a | locked but flagged items exist |
| D-MED-1 | Media present and valid | missing media in the active scene | missing elsewhere |
| D-TXT-1 | Unpublished text drafts | n/a | any |
| D-CAM-1 | Camera selected but unavailable | n/a | any, or published by accident |
| D-MID-1 | MIDI mappings and feedback alarms | n/a | unavailable or alarmed |
| D-LGT-1 | Patch conflicts | conflicts and Arm requested | any conflicts |
| D-LGT-2 | Lighting armed state | n/a | armed at show load |
| D-REC-1 | Recording folder writable and free space | not writable | under 2 GiB |
| D-REC-2 | FFmpeg found, version, encoders | A/V requested, no encoder | n/a |
| D-CST-1 | Broadcast endpoint and secret | n/a | missing |
| D-PRF-1 | Benchmark recent for this config | n/a | none or old |
| D-SHW-1 | Unsaved changes and autosave age | n/a | more than 5 min unsaved |
| D-SHW-2 | Crash recovery pending | n/a | yes |
| D-THR-1 | Thread heartbeats | n/a | any silent for over 2 s |
| D-NET-1 | Listening sockets at idle | any listening | n/a |

Each Blocker and Warning has a one-line cause and a Fix or Details action. Fix jumps to the right control.

## Troubleshooting

### The app will not open

| Platform | Likely cause | What to do |
|---|---|---|
| Windows | SmartScreen warning (unsigned beta) | More info, then Run anyway. See [Getting Started](Getting-Started). |
| macOS | Quarantine flag on the download | `xattr -dr com.apple.quarantine StageSignal.app`, or right-click, then Open |
| Linux | Missing ALSA or X11 libraries | Install the runtime libraries listed in [Building](Building) (the CI deps list) |

### The plugin does not show up in my host

- Check the file is in the standard CLAP folder for your OS. Rescan plugins in the host.
- Hosts differ. Only the hosts the team has run are claimed to work (see [Known Limitations](Known-Limitations), D-04).
- The plugin is an audio effect. Some hosts only send MIDI to instrument-type plugins.

### No audio in the analyser

- Check the input route and that the device is not in use by another app.
- In plugin mode, check the track is routed to the plugin's input and is not muted.
- The Show Doctor D-AUD-1 check reports silence after 5 seconds.

### Lights do not respond

1. Is lighting armed? A disarmed lighting engine sends nothing.
2. Is a blackout latched? Check the bottom strip and press Restore.
3. Is the destination and interface right? Art-Net and sACN ignore a mismatched universe.
4. On macOS, did you deny Local Network access? Grant it in System Settings.
5. Did a fault or reconnect happen? After one, Arm is required again.

Remember that no packet-level check has been done on real fixtures yet (see [Lighting and DMX](Lighting-and-DMX)).

### Recording stopped or failed to start

- FFmpeg missing: install it, put it on `PATH` or next to the app. D-REC-2 shows what was found.
- Disk low: under 256 MiB stops recording. Free space and start again. Recovery offers Keep, Repair or Delete for `.part` files.
- Folder not writable: pick another folder on the Record tab.

### Video is out of sync

Sync between picture and sound is not measured (L-05). Test with a flash-and-click before relying on it.

### Something was silent for a while

The thread heartbeat check (D-THR-1) warns when a worker has been silent for over 2 seconds. Open Diagnostics and the event journal. A support bundle can be exported. Nothing is sent anywhere. You attach it to a report yourself.

## Support bundle

The bundle is a local zip you choose to make. It holds app info, a system summary, the event journal and logs, and capabilities by default. Show structure and settings are optional. Show Report is off by default.

Raw audio, camera frames, keys, full media and crash dumps are rejected. Every item is redacted and then scanned again. You see a preview before anything is written.
