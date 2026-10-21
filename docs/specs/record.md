# Module spec: record (MOD-11 session logic)

Namespace `ss::record`, directory `core/record`, depends on `base`, `model`. Portable C++17, no threads, no ffmpeg
calls, no process spawning. The module owns the **logic** of a recording: the immutable profile, Preparing checks,
the session state machine, disk thresholds, segmentation, naming, the crash-safe journal and recovery scan, the
audio-only WAV writer, and the argv for the *external* ffmpeg process (DEC-12: ffmpeg is never linked or bundled).
The app supplies I/O (file system, process, free-space probe, encoder probe) through small interfaces or plain data.

Dependency note: the task also lists `dynamics`. Its headers are **not included**. The recorder takes plain inputs
instead: `AudioBlockView` (base type) samples, `DynamicsMetadata` (a plain struct of settings/activity for the session
metadata) and `RecordProfileSpec::audioDelayCompensationNs` (a number the dynamics owner supplies).

## Requirement ids

| Id | Covered by |
|---|---|
| REQ-R01 | `RecordProfile`, naming, markers, session metadata, segmented Matroska + PCM + H.264/MJPEG command. |
| REQ-R02 | `DynamicsMetadata` written to `metadataJson` (compressor/limiter settings, activity, clip events, delay). |
| REQ-R03 | One session clock: show/scene events never reset it (`noteShowEvent`); audio PTS delay compensation. |
| REQ-R04 | `DiskMonitor`/`RateEstimator`, warnings and stop reserve, segmentation, journal, recovery scan, writer-fault states. |
| REQ-R05 | Audio-only path (`AudioOnlyRecorder`) needs no renderer, encoder or ffmpeg; exact sample segment boundaries. |
| REQ-R06 | `prepareChecks` verifies path, encoder, free space, sources and the immutable profile before Recording is claimed. |
| REQ-B04 (part) | `SinkFanout`/`ConsumerQueue`: slow network never stalls disk (shared with broadcast). |

## Profile and encoders (section 12)

* `RecordProfile::create(spec)` validates and freezes the spec (`shared_ptr<const>`) and computes a SHA-256 over the
  encoding-relevant fields. Kinds: `AudioOnlyWav` (24-bit PCM WAVE_FORMAT_EXTENSIBLE) and `VideoMatroska`
  (segmented Matroska, 24-bit PCM, H.264 where eligible, MJPEG fallback).
* Encoder probe data is passed in (`ProbeResults{ffmpegPresent, muxer/encoder flags, EncoderInfo{listed,verified}}`);
  `parseFfmpegEncoders` turns `ffmpeg -hide_banner -encoders` text into entries. Eligible: `h264_nvenc`,
  `h264_qsv`, `h264_amf`, `h264_videotoolbox`, `h264_mf`, `h264_vaapi`, `libopenh264`, native `mjpeg`.
  **`libx264`, `libx264rgb`, `libx265` are never selected or assumed** (GPL); builder and selector both reject them.
  An encoder counts only if it is both listed and verified by a short test encode on this machine.
  Policy `PreferH264FallbackMjpeg` picks the first usable hardware H.264, then `libopenh264`, then MJPEG with an
  operator-facing note (`fallbackFromH264`); `H264Only` fails instead of falling back.

## Session state machine (`RecordSession`)

`Idle -> Preparing -> Recording -> Stopping -> Completed`; from Preparing/Recording/Stopping a fault goes to
`Failed` (nothing usable) or `Recoverable` (completed segments exist). `reset()` returns a terminal state to Idle.
`Recording` is claimed only when `completePrepare` returned `ok` (REQ-R06); until then no clock runs. Stop reasons:
User, DiskLow, WriterFault, Shutdown; a stop that is not User sets `endedEarly` in the summary.

Preparing checks (`prepareChecks(profile, PrepareInputs)`), each issue has `blocking`, a message, and an actionable
hint without paths: output path exists/writable, disk probe valid, free space >= reserve + 60 s at the pessimistic
nominal rate, ffmpeg and muxer present (video only), encoder selected, audio source present with matching rate and
channels, video source present with matching size and (when known) frame rate within 0.5 FPS (video only). The report
carries the hash of the immutable profile that was verified, and the session keeps that same frozen profile, so the
settings cannot change between Preparing and Recording. Low-but-sufficient disk and MJPEG fallback are warnings.

## Disk (REQ-R04)

* Rate: `RateEstimator` over a sliding window (<= 120 samples); with < 5 s of samples the nominal profile rate and a
  pessimistic uncertainty are used (`measured=false`); afterwards the measured rate with a ~95% relative uncertainty.
* Warn when free < 2 GiB **or** the pessimistic remaining time (`rate * (1 + uncertainty)`) < 10 min.
* Stop reserve `max(256 MiB, 30 s of bytes at the upper rate)`; at/below it `checkDisk` returns `StopNow` and the session
  moves to Stopping with reason DiskLow so the writer can close files gracefully.

## Segmentation and naming

* Default 15 min (10 s..60 min allowed) or the size budget, whichever comes first. Size budgets: WAV 3500 MiB (hard
  limit below the RIFF 4 GiB ceiling), video 2048 MiB (below FAT32 4 GiB).
* Audio: `AudioSegmenter` cuts on exact sample counts (`samplesToNs`), so boundaries have no overlap or gap, also at
  44.1 kHz. Video: `videoSegmentDuration` derives the cut time from the size budget and the upper rate; ffmpeg's
  segment muxer cuts at keyframes and the builder forces keyframes at the segment interval for H.264.
* File names: `<UTC compact timestamp>_<session-id>_<sanitised show name>_seg<NNNN>.{wav|mkv}`, plus `.journal`,
  `.segments.csv` and `.session.json` with the same stem. `sanitizeShowName` keeps ASCII letters, digits, `.` `-` `_`
  (everything else becomes one `_`), trims leading/trailing `_ .` and spaces, caps at 48 bytes, falls back to `show`
  and prefixes `_` to reserved device names; it never yields separators.
* Markers (`addMarker`/`addMarkerAt`, max 10000, label <= 128 code points) are stamped with the session clock, mapped
  to a segment and offset; exported as CSV and ffmetadata chapters.

## Journal and recovery

* `JournalWriter` appends one line per event: `SSJ1 <crc32 hex> <json>` (Begin, Open, Progress heartbeat after each
  durable flush every 1 s, Close, Marker, Gap, End). `FileJournalSink` does fwrite + fflush + fsync/_commit.
* `parseJournal` tolerates a torn last line (`tornTail`) and counts corrupt lines elsewhere.
* `recoverFromJournal(text, IRecoveryFs&)` repairs **only what is provably complete**: closed segments with matching
  size are `Complete`; the active WAV segment is `RecoveredWav` (header sizes patched, trailing partial frame
  truncated, never extended); an unclosed Matroska segment is `Unverified` (kept, not claimed playable);
  `Missing`/`Mismatch` are reported. `LostInterval` discloses where recoverable data ends and the maximum unflushed
  amount (<= flush interval). Notes are operator-facing text.
* WAV writer: 68-byte header, sizes patched at every 1 s flush, so a crash loses at most ~1 s and the file stays
  readable by tools that trust the header. Capture discontinuities are filled with silence and journaled (`Gap`);
  overlaps are trimmed. Disk-full surfaces `DiskFull`, earlier segments stay recoverable.

## ffmpeg command (`FfmpegCommandBuilder::buildRecord`)

Returns `std::vector<std::string>` argv (argv[0] = ffmpeg path); there is no shell string anywhere. Endpoints must be
`pipe:<n>`, a Windows named pipe or an absolute path; URLs, leading `-`, control chars are rejected.

Verified against ffmpeg 6.1.1 (local run) and the ffmpeg-formats / ffmpeg-protocols documentation:

* `-f rawvideo -pix_fmt rgba|bgra -video_size WxH -framerate N/D -i <endpoint>`; `-f f32le -ar R -ac C -i <endpoint>`;
  `-thread_queue_size 512` per pipe input; `pipe:N` reads descriptor N on POSIX.
* `-itsoffset X` applies to the **next** `-i` and delays that input by positive X. The builder computes
  offsets so that audio PTS = (audio start - delay compensation) relative to video; negative values are shifted onto the
  video input so every offset is >= 0. A local run with a 5 ms delay compensation was checked with ffprobe stream
  start times and showed the expected shifted audio offset (unit test `REQ-R03: audio PTS offset ...` pins the numbers).
* Segmenting: `-f segment -segment_format matroska -segment_format_options cluster_time_limit=1000:flush_packets=1
  -segment_time S -segment_start_number 1 -reset_timestamps 1 -segment_list <csv> -segment_list_type csv
  -segment_list_flags +live`, plus `-force_key_frames expr:gte(t,n_forced*S)` for H.264. A local 25 s run with
  S = 10 produced three segments with exact 10/20/25 s boundaries in the CSV list.
* Encoders: hardware H.264 `-b:v/-maxrate/-bufsize/-g`, `-pix_fmt yuv420p`; MJPEG `-q:v -pix_fmt yuvj420p`;
  audio `pcm_s24le`. `-hide_banner -nostdin -nostats -loglevel warning` as globals.
* The shared pieces live in `ss::record::ff` (`computeInputOffsets`, `checkEndpoint`, `appendInputs`,
  `appendVideoEncode`, `appendAudioEncode`, `appendForcedKeyframes`, `checkArgv`) and are reused by `broadcast`.

## Record while broadcasting

`SinkFanout(diskCap, netCap, encodedPackets)` is a non-blocking publisher with two bounded `ConsumerQueue`s. The
network queue drops oldest (freshness) and can require a keyframe resync; the disk queue drops newest and reports
`LossRange`s into `noteLoss` so the session summary and journal disclose them. Both queues share `shared_ptr` payloads
(no copy). `planCoexistence` documents the decision: two **independent** ffmpeg processes/muxers (no shared encoded
packets in this beta), `encoderInstances = 2` for the admission/perf module, reasons listed when the signatures
would have matched. A stuck broadcast cannot delay the recording; memory is bounded by `maxMemoryBytes()`.

## Tests (`tests/unit/record/test_record.cpp`, 40 cases, doctest)

REQ-named: profile validation and hash; encoder text parsing and selection; every Preparing failure and warning;
state machine and "Recording only after Preparing"; clock not reset by show events; reserve formula; measured rate
and uncertainty; warn/stop thresholds; disk-low stop; writer fault states; file naming; exact audio segmentation
(48 kHz and 44.1 kHz, size-budget cut); video segment duration; segment CSV continuity; audio-only start without
renderer/encoder/ffmpeg; WAV header; exact segment recorder with journal; discontinuity fill; disk-full recovery; header
recovery after simulated power loss with lost-tail disclosure; torn journal; real files via `StdioRecorderFs`; markers
and cap; dynamics metadata; argv structure (H.264 and MJPEG); PTS offset; unsafe endpoint/GPL rejection; fan-out
isolation, bounds, keyframe resync and coexistence plan. No test needs ffmpeg or a device.

## Status (honest)

Complete and unit-tested: everything above.

Partial or absent:
* **No drift controller.** Clock-drift correction of the audio versus video clocks (+-200 ppm resampling) is not
  implemented; the builder assumes constant-frame-rate video and a stable audio rate from the caller.
* **No RF64.** WAV segments are cut below 4 GiB instead; very large single-segment WAV is not supported.
* **No hardware qualification or encoder tuning.** Which GPU encoder is *good* on a given machine is decided by the
  app's verification encode and the probe flags; presets/rate-control tuning per vendor is untouched.
* **No transitions or compositing** (render/show own them); the recorder records the frames it is given.
* **Real ffmpeg was exercised only locally and by hand** (argv fed through FIFOs, segment boundaries and offsets
  checked); no CI test needs it. Process supervision of the record ffmpeg (spawn, pipe feeding, crash restart) is
  the app's job; no `IProcess` is defined in this module (the broadcast module has one).
* Matroska segments recovered after a crash are `Unverified` by design (remux needed); only WAV is repaired in place.
* `SinkFanout` shares no encoded packets between record and broadcast: two encoders run when both are active.
* The WAV writer here duplicates what the dynamics/audio side may write for its own files; the integrator should
  reconcile if a shared writer is wanted.
* Free-space values come from the app; a network or removable drive can lie about free space.
* Unit tests only (doctest). No soak or power-loss test on real hardware.

## Integration API

```
#include "ss/record/{profile,prepare,session,disk,segment,journal,wav,naming,ffmpeg_args,fanout}.hpp"   // ss::record
// Setup:    RecordProfileSpec s; s.kind = AudioOnlyWav | VideoMatroska; s.audio={48000,2}; s.video={w,h,fpsN,fpsD,...};
//           s.outputDir = "<abs folder>"; s.audioDelayCompensationNs = <from dynamics>;  auto p = RecordProfile::create(s);
// Prepare:  RecordSession rs(clock);  rs.beginPrepare(p.value(), makeSessionId(rng));
//           PrepareInputs in{ PathProbe, DiskProbe, ProbeResults(ffmpeg -encoders + verify encode), SourceProbe };
//           PrepareReport r = rs.completePrepare(in);    // r.ok -> state Recording; r.issues -> show message + action
// Audio:    AudioOnlyRecorder rec(profile, rs.summary().sessionId, wallStartNs, IRecorderFs&, IJournalSink&);
//           rec.start(); per audio block: rec.write(AudioBlockView) ; rec.stop(clean);   (StdioRecorderFs, FileJournalSink given)
//           feed rs: onSegmentOpened/Closed(index,...), onBytesWritten(total), noteLoss(...), onWriterStopped(clean)
// Video:    RecordCommandInput ci(profile); ci.encoder = r.encoder.value(); ci.video/audio = {pipe or fifo endpoints};
//           ci.sync = {videoStartHostNs, audioStartHostNs}; ci.sessionId, ci.sessionStartWallUnixNs; ci.ffmpegPath;
//           auto cmd = FfmpegCommandBuilder::buildRecord(ci);  spawn cmd.value().argv with posix_spawn/CreateProcess (no shell);
//           feed raw RGBA frames + f32le PCM into the endpoints; poll cmd.segmentListPath -> parseSegmentListCsv / checkContinuity.
// Running:  every ~1 s: DiskStatus d = rs.checkDisk(freeBytes);  Warning -> notify; StopNow -> rs.requestStop(DiskLow) (done by checkDisk)
//           markers: rs.addMarker("label") / addMarkerAt(label, mediaNs);  show/scene change: rs.noteShowEvent() (clock untouched)
//           stop: rs.requestStop(StopReason::User); writer finishes; rs.onWriterStopped(clean) -> Completed
//           faults: rs.onWriterFault(err, lengthKnown, lostNs) -> Recoverable | Failed
// Metadata: rs.metadataJson(DynamicsMetadata{...}); markersCsv(markers); markersFfmetadata(markers, totalNs)
// Recovery: at app start, for each *.journal: recoverFromJournal(text, StdioRecorderFs(dir)) -> RecoveryReport
//           show report.notes (lost tail disclosure); segments Unverified need remux; never claim them playable.
// Coexist:  SinkFanout fan(diskCap, netCap); producers call fan.publish(chunk) (never blocks); disk consumer drains fan.disk(),
//           broadcast drains fan.network(); fan.setNetworkEnabled(on); losses: fan.disk().takeLosses() -> rs.noteLoss(...)
//           planCoexistence(recordSig, broadcastSig).encoderInstances (=2) -> report to the admission/perf module
// Needs from others: free-space probe, folder-writable probe, ffmpeg probe + verify encode, process spawn/pipe feeding,
//           capture/dynamics audio blocks and the audio delay compensation value, a media clock (IClock).
// Errors: ss::Result/Status; DiskFull, EncoderUnavailable, InvalidArgument, TransportFailure. Texts contain no paths.
```
