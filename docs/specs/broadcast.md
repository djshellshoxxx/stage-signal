# Module spec: broadcast (MOD-12 session logic)

Namespace `ss::broadcast`, directory `core/broadcast`. Declared dependencies: `base`, `model`, `record` (see
"Dependency edges"). Portable C++17, no threads, no network, no process spawning in this module. It owns settings and
validation, the publication confirmation (start token), the session state machine with reconnect, the argv for the
**external** ffmpeg process, the interpretation of ffmpeg's output, redaction, and the status the UI shows. The app
supplies process spawning (`IProcessFactory`), the media feed (`IMediaFeed`) and secret resolution (`ISecretResolver`).

## Dependency edges (integrator action needed)

* `broadcast -> record`: the module reuses `ss::record::ff` (input offsets, endpoint check, encoder/keyframe argv
  pieces, argv check), `EncoderChoice`/`isEligibleEncoder`, and `SinkFanout` (the bounded network queue). The task
  listed `base, model, security`; **`docs/ARCHITECTURE.md` needs a note for this extra edge** (not edited here).
* `security` headers are **not included**. Secrets are modelled by a plain `SecretRef` ("ref:<id>") and an
  `ISecretResolver` interface (`exists`, `resolve`) that the security/app layer implements.

## Requirement ids

| Id | Covered by |
|---|---|
| REQ-B01 | Profiles, settings validation and defaults, publication summary, command builder (RTMPS/RTMP/SRT). |
| REQ-B02 | `StartToken` issued only from user actions and bound to the confirmed summary; single use, TTL 15 s. |
| REQ-B03 | `SecretRef`, redaction, portable exports without secrets, status secret report. **Partial**, see Status. |
| REQ-B04 | Live semantics, reconnect schedule and deadline, terminal errors, stall/connect timeouts, shared fan-out. |
| REQ-B05 | Stop within 1 s, session generation so stale callbacks cannot restart a stopped stream. |
| REQ-B06 | Offline start keeps retrying visibly; network trouble never touches recording. |
| REQ-B07 | RTP documented as Later (unavailable), WebRTC Deferred; both refused by validation and builder. |
| Section 15 | Defaults, BASE fallbacks, upload warning, encryption labelling, camera/audio publication summary. |

## Profiles and settings (REQ-B01)

| Profile | Availability | Output | Protection shown to the operator |
|---|---|---|---|
| RTMPS | Beta | FLV over TLS | "encrypted (TLS, certificate and host name verified)": validation is **mandatory** |
| RTMP | Optional | FLV | "NOT encrypted"; start needs `acknowledgeUnencrypted` |
| SRT caller | Beta | MPEG-TS over SRT | "encrypted (AES-128/192/256)" with passphrase, else "NOT encrypted" + acknowledgement |
| RTP | Later | none | documented only: needs a receiver SDP/codec profile; RTP/RTCP without SRTP is unencrypted |
| WebRTC | Deferred | none | signalling, NAT traversal and congestion control are out of scope for the beta |

Defaults: 1280x720 @ 30 FPS, 2500 kb/s H.264, AAC stereo 48 kHz 128 kb/s, keyframe every 2 s, reconnect on. Validation
(`validateSettings(s, baseTier)`): host only (no scheme/userinfo/path/query; IPv6 in brackets), port 1..65535 (SRT
requires one), RTMP application path with a safe charset and not empty, secret fields must be `ref:` references (a
pasted key or URL is rejected rather than stored), SRT latency 20..8000 ms, connect timeout 500..30000 ms, key length
16/24/32, video 160x120..1920x1080 even sizes, 5..60 FPS, 300..20000 kb/s, keyframe 1..10 s, audio 44.1/48 kHz,
mono/stereo, 64..320 kb/s. BASE adds non-blocking advice. `applyBaseFallback(s, use640x480, fps 15..25,
receiverAcceptsFallback)` gives 800x600 or 640x480 only when the receiver is confirmed to accept it.
`requiredUploadKbps` = (video + audio) * 1.10; `uploadWarning(s, measured)` explains that the stream will stutter
while recording is unaffected. `exportSettingsJson` (portable) carries label, endpoint, encoder settings and
encryption state but **no reference and no secret** (only "key set" flags); `serializeSettingsJson` is the local-store
form with references.

## Start gating (REQ-B02)

`confirmPublication(ActionSource, PublicationFacts)` refuses `Show`, `System`, `Remote`; accepts `Ui`, `Keyboard`,
`Midi`. It returns a `StartToken` (private constructor) holding the SHA-256 of `PublicationSummary::text()` (endpoint,
protection, video, audio, audio source, program output, camera included or not, recording running). `start(token, facts,
media)` refuses an unknown, reused, expired (15 s) token, or one whose summary hash differs from the current
settings/facts. Show load, restart recovery and benchmark code have no path that creates a token, so none can go live.

## State machine and Live semantics (REQ-B04)

`Off -> Preparing -> Connecting -> Live -> Reconnecting | Stopping -> Off`, `Error` for terminal failure.
Preparing validates settings, builds the command, checks that referenced secrets exist; nothing is spawned if it fails.
**Live** is claimed only after two progress samples (`-progress pipe:1`) with growing `total_size` and `out_time > 0`;
process start alone is Connecting. Limits: a Connecting attempt with no outbound media for 20 s, or a Live stream
whose bytes stop growing for 10 s, counts as a failure (process killed).

Failure handling (`classifyStderr/classifyExit`, patterns taken from real ffmpeg 6.1.1 output):
* **Terminal, never retried:** certificate (`Peer certificate failed verification`...), authentication (publish
  denied/BadName, 401/403, stream key rejected), invalid configuration (unknown option, `Option not found` exit 8,
  codec not compatible with FLV, bad passphrase), ffmpeg not startable. State becomes `Error`, the feed is
  deactivated and the queued media dropped.
* **Retryable:** DNS failure, connection refused, timeout, connection lost/handshake unanswered, stall, unknown.

Reconnect: delays 1, 2, 4, 8, 15, 15... s, each multiplied by 1 +- 20% jitter from the injected `Rng`, measured on the
injected `IClock`; a hard 120 s deadline from the **first failure of an outage** (an attempt in flight is also killed
at the deadline). A connection that stays Live for 30 s clears the outage and the attempt counter; a flapping
connection keeps escalating. At every disconnect and every attempt, `IMediaFeed::abandonQueued()` drops stale media and
`requestKeyframe()` is called; the new ffmpeg process also starts with a fresh GOP.

## Stop and stale callbacks (REQ-B05)

`stop()` bumps the session **generation**, deactivates the feed, asks the process to stop (`requestStop`) and kills it
after a 750 ms grace, so the state is `Off` within 1 s when `tick()` is driven (Reconnecting/Error/Preparing go Off at
once). `onReconnectTimer(gen)` and `onProcessExited(gen, ...)` ignore any generation but the current one (also logged
as "ignored stale ..."), so an in-flight timer or exit report cannot restart a stopped stream or act for an older
process. Reconnect attempts and failures that end a session also bump the generation.

## Command line (`FfmpegCommandBuilder::buildBroadcast`)

Output is a `BroadcastCommand` of `ArgPiece`s (text, or text around a `SecretRef`). `displayLine()` is the only form
that is logged or shown (URL path/key and passphrase redacted); `materialize(resolver)` resolves secrets at spawn
time, validates their form (control characters, length; SRT passphrase 10..64) and the session wipes the temporary
copies after `IProcess::start`. Shared pieces from `ss::record::ff`: raw `rgba|bgra` video + `f32le` PCM pipe inputs with
`-itsoffset` (audio delay compensation from the caller), `-thread_queue_size`, H.264 `-b:v/-maxrate/-bufsize/-g`,
AAC `-c:a aac -b:a`, `-force_key_frames expr:gte(t,n_forced*2)`. Requires an eligible H.264 encoder (MJPEG and
libx264 are refused) and inputs equal to the stream size.

* RTMPS: `-tls_verify 1 -verifyhost <host> -f flv rtmps://host:443/app/<KEY>`
* RTMP: `-f flv rtmp://host:1935/app/<KEY>`
* SRT: `-mode caller -latency <ms*1000> -connect_timeout <ms> [-passphrase <P> -pbkeylen N -enforced_encryption 1]
  -mpegts_flags +resend_headers -f mpegts srt://host:port`
* all: `-progress pipe:1 -stats_period 1.000000` before the output.

### ffmpeg options verified (ffmpeg 6.1.1, Ubuntu build with gnutls, libsrt, libx264; local `-h` output and real runs)

* `tls_verify` exists on the tls protocol with **default 0** (no validation): the builder forces `1` and passes `verifyhost`
  (`-h protocol=tls` lists `ca_file`, `tls_verify`, `verifyhost`). With a self-signed server the real run failed with
  `Peer certificate failed verification` and exit 251, confirming that validation is enforced when set.
* SRT private options accepted as output options before the URL: `mode`, `latency` (**microseconds**), `connect_timeout`
  (**milliseconds**), `passphrase` (10..64 chars, 0 disables crypto), `pbkeylen` {16,24,32}, `enforced_encryption`.
  A local run with all of them reached the connect stage (refused, no server) without "unrecognized option".
* Muxer options `flvflags` and `mpegts_flags resend_headers` exist; FLV rejects MJPEG (`not compatible with flv`).
* `-progress pipe:1 -stats_period S` prints `key=value` blocks ending in `progress=continue|end`; keys seen:
  `frame fps stream_0_0_q bitrate total_size out_time_us out_time_ms out_time dup_frames drop_frames speed`; `bitrate`
  and `out_time*` are `N/A` before the first packet. (`out_time_ms` is also microseconds; the parser uses `out_time_us`.)
* Failure texts used by the classifier were captured from real runs: TLS verification, `Connection refused`,
  `Failed to resolve hostname ... Name or service not known`, `Cannot read RTMP handshake response`, `Input/output error`,
  `Unrecognized option` (exit 8). ffmpeg echoes the full output URL, including the stream key, in several of them;
  everything read from the process is redacted before it is stored.
* **Not verified:** an actual publish to an RTMPS/RTMP/SRT server (none available), certificate and host-name checking
  on non-GnuTLS TLS backends (OpenSSL, Schannel, SecureTransport builds), hardware-encoder option behaviour.

## Secrets and redaction (REQ-B03)

Settings hold only `SecretRef`s; exports contain no reference and no secret; logs, status and error text pass through
`redactText` (URL userinfo, path and query removed, host/port kept). ffmpeg errors that echo the URL are redacted
before storage. A missing or malformed secret blocks the start with an actionable message and never retries.

## Status struct

`BroadcastStatus{state, connection text, encryption state + label, measured bitrate (kb/s from total_size deltas), bytes
sent, dropped frames (ffmpeg drop_frames across processes + chunks dropped by the feed), reconnect attempts/total,
outage elapsed, next attempt in, generation, last 10 errors (kind, redacted message, terminal), secrets report}`.

## Tests (`tests/unit/broadcast/test_broadcast.cpp`, 63 cases, doctest; no ffmpeg, no network, no sleeping)

REQ-B01: defaults, availability, validation of every field, encryption labelling, BASE fallbacks, upload warning,
RTMPS/RTMP/SRT argv (TLS flags, units, order), passphrase length, rejection of MJPEG/libx264/mismatched inputs/URL
inputs/option-like host, shared `-itsoffset` delay compensation, summary and hash, settings round trip.
REQ-B02: Show/System/Remote refused, no token no start, UI/keyboard/MIDI accepted, single use, expiry, bound to summary,
replacement, double start. REQ-B03: redaction, exports, display line, URL-echoing stderr, secret only in the spawned
argv, status says PARTIAL, missing secret, secret vanishing before reconnect. REQ-B04: Live only after growth,
state sequence, measured status, schedule 1/2/4/8/15 with jitter bounds and the 120 s deadline, deterministic
jitter from the rng, fresh queue/keyframe per attempt, terminal vs retryable failures, classifier on real texts,
reconnect disabled, stable vs flapping, stall, connect timeout, unexpected clean exit, progress parser. REQ-B05:
cooperative and hung stop under 1 s, stop while reconnecting, stale timer and stale exit generations, idempotence.
REQ-B06: offline start then recovery; with the shared `SinkFanout` a stalled network queue loses nothing on disk.
REQ-B07: RTP/WebRTC refused. T-FAULT: not startable ffmpeg, crash then retry with accumulated counters, bounded error
list, fake process behaviour.

## Status (honest)

Complete and unit-tested: settings, validation, token gating, state machine, reconnect, classifier, progress parser,
command builders, redaction, status, `IProcess` fake, fan-out adapter.

* **REQ-B03 / section 15 secret handling is PARTIAL, not solved.** Secrets are never stored in settings, exports, logs
  or status, and are resolved only at spawn, but ffmpeg receives the stream key (in the URL) and the SRT passphrase
  (as an option) **on its command line**, which other processes of the same user, and administrators, can read while
  the stream runs (`/proc/<pid>/cmdline` on Linux, process listings on Windows/macOS). Mitigations documented, none
  removes the exposure: resolve late, wipe temporary copies, run the show machine under a dedicated OS account,
  hide process listings where the OS allows (Linux `hidepid`), and prefer SRT passphrase rotation. A real fix needs a
  different transport (a tiny in-process or helper relay that reads the secret from a pipe/environment, or linking a
  protocol library), which this beta does not do. The status struct reports `Partial` with this text so it cannot be
  overlooked. The environment of a child process has the same visibility issue to its own user, so it is not claimed as better.
* **No publish was ever tested against a real server**; reconnect/classifier behaviour on a real RTMPS/SRT service,
  and certificate/host verification on non-GnuTLS ffmpeg builds, are unverified.
* **Redaction of passphrases in free text** relies on not receiving them back from ffmpeg (options are not echoed in
  the messages seen); unknown echoes of a secret as non-URL text would not be caught because the session does not keep
  the resolved values.
* **Live is inferred** from the muxer's output byte count; it cannot prove the server accepted the media (no
  server acknowledgement is visible to ffmpeg's progress output).
* **No drift controller, no encoder-specific tuning, no hardware qualification, no transitions** (as in record).
  Audio/video drift on long streams is not corrected; `-itsoffset` only fixes the start offset.
* Bitrate adaptation is absent: the operator picks a bitrate and gets warned; the stream never self-downgrades.
* Camera/audio publication facts (`PublicationFacts`) are supplied by the app; this module only displays and hashes them.
* RTP is documented but not implemented; WebRTC is deferred. The session is single-endpoint.
* Two independent encoder processes run when recording and streaming together (no shared encoded packets); the admission
  module should count both.
* The process-supervision glue (spawning, pipes, reading stdout/stderr without blocking, feeding frames from
  `SinkFanout::network()`) is the app's job; `FakeProcess` documents the contract only.
* `broadcast -> record` dependency edge needs an `ARCHITECTURE.md` note.
* Unit tests only (doctest).

## Integration API

```
#include "ss/broadcast/{settings,session,command,process,ffmpeg_io,redact}.hpp"       // link ss::broadcast (pulls ss::record)
// Implement:  IProcessFactory/IProcess (non-blocking: start(argv), running(), exitCode(), readProgress() = ffmpeg stdout,
//             readStderr(), requestStop() graceful, kill());  IMediaFeed (use FanoutFeed over record::SinkFanout);
//             ISecretResolver { exists(ref); resolve(ref) } backed by the security module (OS keychain / DPAPI).
// Settings:   BroadcastSettings s = defaultSettings(); s.profile = Rtmps; s.host = "live.example.com"; s.path = "live";
//             s.streamKey.handle = "ref:<id from secret store>";   auto issues = validateSettings(s, baseTier);
//             hasBlocking(issues) -> disable Start; show issue.message; applyBaseFallback(...); uploadWarning(s, measuredKbps)
//             store: serializeSettingsJson(s) (refs, local only) / parseSettingsJson;  share: exportSettingsJson(s) (no refs)
// Session:    BroadcastSession bs(s, clock, rng, processFactory, feed, secrets, SessionConfig{});
// Confirm:    PublicationFacts f{cameraPublished, "Master bus (after limiter)", "Output 1 - Main", recordingActive};
//             show makeSummary(s, f).text() in the dialog; on the OPERATOR's click/key/MIDI:
//             auto t = bs.confirmPublication(ActionSource::Ui | Keyboard | Midi, f);   // Show/System/Remote are refused
// Start:      BroadcastCommandInput m; m.ffmpegPath; m.encoder = <verified H.264 EncoderChoice from record prepare>;
//             m.video = {endpoint, w,h,fps from s.video}; m.audio = {endpoint, 48000, 2}; m.sync = {videoStart, audioStart, delayComp};
//             bs.start(t.value(), f, m);       // within 15 s of confirmation, same settings and facts
// Each tick:  pump frames from fan.network() into the process endpoints; bs.tick();   (~every 100 ms, control thread)
//             BroadcastStatus st = bs.status();   state, connection, bitrateKbps, droppedFrames, errors, encryptionText, secrets.note
// Stop:       bs.stop();  keep calling bs.tick() until state()==Off (<= 1 s);  bs.log() is already redacted
// Event APIs: bs.onProcessExited(gen, code, stderr) / bs.onReconnectTimer(gen) with the generation read from status().generation
// Coexist:    record::SinkFanout fan(diskCap, netCap); FanoutFeed feed(fan); recording drains fan.disk(), broadcast fan.network()
// Tests:      FakeProcessFactory, FakeProcessData::{emitProgress,crash}, FakeFeed, FakeClock, Rng(seed)
// Needs:      process spawn + pipe plumbing, secret store, verified H.264 encoder, program-output frames + mixed PCM,
//             connectivity/upload measurement (optional), ARCHITECTURE.md edge note (broadcast -> record).
// Errors: Result/Status with PermissionDenied (not a user action, bad token, missing secret), InvalidArgument,
//         UnsupportedCapability (RTP/WebRTC), EncoderUnavailable. Texts and logs are redacted.
```
