# diag (MOD-16 + Show Report), spec

Scope: bounded event journal with markers, tempo/key sampler (<= 1 Hz) and feature history, log rotation planner (5 x 5 MiB),
support-bundle composer with preview and redaction scan, Show Report (HTML + CSV). REQs: REQ-X05, REQ-B03, DEC-13 Show Report
(spec sections 16, 17; T-DATA, T-SEC). Namespace `ss::diag`; depends on base, model, security.

## Components
| Header | Purpose |
|---|---|
| `journal.hpp` | `EventJournal(IClock&, capacity)`: ring (16..100,000 events), each with `seq`, `monoNs`, `wallUnixMs`, `EventKind`, `id`, sanitised `detail` (redacted, one line, <= 256 bytes). `addMarker`, `markers()`, `snapshot()`, `overwritten()`, `toModelEntries()` for the show document. |
| `sampler.hpp` | `TempoKeySampler` (emits at most one `FeatureSample` per second of monotonic time; backwards time never emits) and `FeatureHistory` (ring, default 6 h). Unknown tempo/key are stored as `bpm=0`, `keyTonic=-1`. |
| `logrotate.hpp` | `planRotation` (5 files x 5 MiB, `stagesignal.log`, `.1.log` ... `.4.log`), `planRetention`, `planDeleteAll`, `formatLogLine` (redacted, one line, <= 2000 bytes). Plans are lists of Delete/Rename ops for the app to apply. |
| `support_bundle.hpp` | `SupportBundleComposer`: `addItem`, `setSelected`, `preview()` (categories, counts, bytes, redaction counts, first redacted lines), `compose()` (redacted files + `manifest.txt`), `scanBundleForSecrets`. |
| `show_report.hpp` | `ShowReportBuilder` -> `ShowReport{html, eventsCsv, setlistCsv, summary, setlist, sceneUsage, drops, truncated}`. |
| `text.hpp` | `htmlEscape`, `csvText` (RFC 4180 + formula neutralising), `fixedDecimals`, `formatUtcIso`, `formatDuration`, `keyName` (locale-free). |

## Support bundle rules
Allowed categories: app info, system summary, event journal, logs, capabilities (default on); show structure, settings (redacted),
show report (default off, opt-in). Raw audio, camera frames, key material, full media and crash dumps are rejected by category, by
file extension (wav, mp3, mp4, png, jpg, dmp, pem, key, zip, ...), by content (NUL bytes, invalid UTF-8, `-----BEGIN`, RIFF/PNG/JPEG/ZIP
magic). Limits: 64 items, 8 MiB per item, 32 MiB total. Every item is redacted with patterns plus the secret-store literals, then
re-scanned; non-convergence fails closed. The operator sees `preview()` before `compose()`.

## Show Report
Inputs: journal events (scene, marker, device, adapt, rec, cast, gap, error ...) and feature samples (time, bpm, key, energyArc, arcPhase,
sceneId), scene names, title, app version, generated-at wall time (supplied by the caller, so output is deterministic).
- Setlist: one section per marker (start to next marker or session end) with median BPM, most common key, peak energy; with no markers a
  single implicit "Session" section. Drops: samples entering arc phase 2. Scene usage from scene events (else from sample scene ids):
  activations, time, share. Operational table: device, adaptation, recording, broadcast and error events. Discontinuities: journal gap
  events plus derived "no feature samples for N s" (> 5 s).
- HTML: one file, inline CSS (light/dark, print, phone width), inline SVG chart (energy solid, BPM dashed, marker lines, drop dots, gap
  ticks; <= 600 buckets per series), tables. A CSP meta (`default-src 'none'`) forbids anything external; no `<script>`, `<link>`, `<img>`, `href`, `src`.
- Escaping: every dynamic string goes through `htmlEscape` (also inside SVG text); CSV text cells are quoted and a leading `= + - @ tab CR` gets an apostrophe.
- Bounds: 20,000 events and 100,000 samples consumed, 500 rows per table (halved until the page fits 1.5 MiB; `truncated=true` and a
  "N more rows" note), CSV capped at 8 MiB. Same input in any order gives byte-identical output.

## Tests (tests/unit/diag, 30 cases, about 1.5 s)
REQ-X05 journal clocks/ring/markers/sanitising/sampler cadence (10 s at 100 Hz -> 10 samples); REQ-B03 log rotation simulation (100 MiB of
writes never exceed 5 files / 25 MiB), name parsing, retention, support-bundle denial corpus, preview, redaction, size limits; DEC-13 report
golden substrings, determinism and order independence, XSS payload set (titles, markers, scene names, details), CSV injection, empty and
no-marker/no-sample inputs, derived gaps, size bounds with 30,000 hostile events, report built from the real journal and sampler.

## Integration API
```
using namespace ss::diag;
SystemClock clock;  EventJournal journal(clock, 4096);  FeatureHistory history;       // owned by AppController
journal.append(EventKind::Scene, "to Neon Grid", sceneId);        // scene/cue/mode/text/device/adapt/rec/cast/gap/error/note
journal.addMarker("Song A");                                      // ActionId::AddMarker handler
history.offer(clock.nowMonotonic(), featureSnapshot, currentSceneId);   // call per FeatureSnapshot; keeps <= 1 Hz
showDoc.journal = journal.toModelEntries();                       // embed in the show file when saving
// Show Report (ActionId::ExportShowReport):
ShowReportBuilder b;
b.setTitle(showName).setAppVersion(v).setGeneratedAtWallMs(wallNowMs).setSceneNames({{id, name}, ...})
 .addEvents(journal.snapshot()).addSamples(history.samples());
auto rep = b.build();       // rep.value().html / eventsCsv / setlistCsv; write to files chosen by the operator; rep.value().truncated
// Logs: each line  out = formatLogLine(wallMs, "INFO", msg);  before appending:
auto plan = planRotation(listOfLogFiles, LogRotationConfig{}, out.size());   // apply plan.ops in order, then append to plan.activeName
// Support export:
SupportBundleComposer c; c.setRedactor(security::makeRedactorFor(secretStore));
c.addItem(BundleCategory::EventJournal, "journal.csv", report.eventsCsv);  c.addItem(BundleCategory::Logs, "stagesignal.log", tail);
auto pv = c.preview();  /* show categories + sample lines; operator toggles c.setSelected(cat, on) */  auto bundle = c.compose();  // then zip bundle.value().files
```
Threading: journal and history are single-writer; wrap or confine to one thread. Nothing here blocks or touches the filesystem.

## Status
- Done and unit-tested: all of the above (portable logic, Linux gcc 13 and clang).
- The report HTML was checked for well-formed tag nesting with a parser and by golden substrings; it has not been viewed in Windows,
  macOS or Linux browsers or printed, so layout and dark-mode appearance are unreviewed.
- The journal does not detect wall-clock jumps (NTP step, DST is irrelevant as wall time is UTC); both clocks are recorded so a reader can see them.
- Not done (app layer): applying rotation ops to a real directory, zipping the support bundle, collecting system summary/capabilities text,
  and the UI for the bundle preview. Key names in the report use sharps only (C#, not Db).
- Tempo/key "snapshots at <= 1 Hz" are provided by the sampler; the journal's own `Tempo`/`Key` event kinds exist for change events the app may log.
