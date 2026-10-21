# persist (MOD-13), spec

Scope: ShowDocument <-> JSON, validation against `model::Limits`, migration framework, portable bundle planner, `IFileSystem`
(+ in-memory fake with fault injection + std::filesystem implementation), atomic save with 3 backups, debounced autosave, crash
recovery proposal, local settings. REQs: REQ-S13, REQ-S14, REQ-X01 (spec 10 IF-11, 17, 18 T-DATA/T-SEC/T-OFFLINE).
Namespace `ss::persist`; depends on base, model, security. JSON via `third_party/nlohmann/json.hpp` (private include; no JSON in public headers).

## Files and formats
- Show JSON (`show_json.hpp`): `{"format":"stagesignal-show","schema":{"major","minor"},...}` with sorted keys, compact, deterministic.
  64-bit seeds are decimal strings. Enums are strings (`"neonGrid"`), never numbers. Optional `metadata` object and `migrations` array
  ride in `ShowExtras` (the model has no field for them).
- Save file (`store.hpp`): one header line `STAGESIGNAL-SAVE 1 kind=save|autosave saved=<unix ms> bytes=<n> sha256=<hex>` then the show JSON.
  Torn or flipped files fail the length/checksum test. Raw JSON (no header) is accepted on load only.
- Sidecars next to `<path>`: `.bak1 .bak2 .bak3` (previous valid saves), `.recovery` (autosave), `.session` (open lock), `.tmp`,
  `.premigrate-v<N>`, `.restored[-n]`.
- Bundle (`bundle.hpp`): `manifest.json`, `show.json`, `media/<sha256>.<ext>` (ext in png jpg jpeg bmp gif mp4 mkv).
- Local settings (`settings.hpp`): `stagesignal-settings` v1 with device hints, recording folder, last show path and secret references only.

## Validation (before any state is produced)
Input size over 8 MiB is refused before parsing; nesting over 64 refused before parsing; arrays checked against `model::Limits`
(10,000 cues, 1,000 scenes, 256 messages, 512 MIDI, 256 fixtures) before elements are read; journal over 50,000 entries refused.
Semantic checks: unique ids, every cue/scene/media/text/profile/output reference resolves, enum ranges, finite numbers and ranges,
text messages valid UTF-8 and at most 4,096 code points (`ss::utf8`), names at most 256 bytes, patch fits a 512-slot universe,
lighting destination is IPv4, media paths are canonical relative paths (never URLs; empty = not included), sha256 is 64 lowercase hex,
MIDI action ids must exist in the action registry. Schema: newer major -> `SchemaUnsupported`; newer minor -> loads, unsupported
fields reported by name only; older major -> needs a registered migration. Unknown executable behaviour is rejected: keys such as
`script`, `exec`, `command`, `plugin`, `url`, `macro`, `eval` anywhere (also inside `metadata`), unknown scene kinds, transitions,
tiers or action ids. Other unknown keys are ignored with a warning. `metadata` is preserved verbatim (object, <= 64 KiB, depth <= 12).

## Atomic save (REQ-X01)
`ShowStore::save`: serialise+validate (nothing touched on failure) -> remove stale temps -> refuse to overwrite a newer-format file
-> write `<path>.tmp` flushed -> read back, verify checksum, re-validate -> rotate backups (only a *valid* previous file is rotated)
-> rename tmp over path (the commit point) -> drop `.recovery`. A failure anywhere leaves the previous file untouched and returns the error.
`load`: primary, else newest valid backup with an operator note; a newer-format primary is reported, never skipped.
Crash-at-write coverage: every mutating filesystem op of a rotating save is crashed with torn fractions 0, 0.4, 0.99 (and DiskFull for
every op); afterwards `load` returns the old or the new version and the next save works. The fake can drop unflushed data on crash, which
is how a missing flush would be caught.

## Autosave, recovery, dirty state (`session.hpp`)
`AutosaveController` is IClock driven: first change of a dirty period sets the deadline to now+30 s; further edits do not move it. A due
tick writes only `.recovery`. Failure keeps the document dirty, keeps the previous recovery file, records `lastError()` and retries after
another window. `ShowSession::saveNow` failure keeps `dirty()`, keeps the old file, exposes `lastError()`.
`detectRecovery` offers a proposal only for a valid, checksummed `.recovery` that is newer than the saved file or follows an unclean exit
(`.session` left behind). `RestoreProposal` carries `startDisarmed=true`, `lightingArmed/broadcastLive/recordingActive=false`.
`acceptRestore` writes an explicit copy `<path>.restored[-n]` (never over the original), then drops recovery and lock.

## Migration (`migration.hpp`)
`MigrationRegistry` of pure JSON-text -> JSON-text steps. `planMigration` -> `applyMigration`: backup of the exact original bytes first
(flushed, never overwriting an earlier backup), then in-memory steps, schema check after every step, final validation. Load never rewrites
the source; the migrated document is saved only by an explicit save (which keeps the old file as backup 1). Newer files are never migrated.

## Tests (tests/unit/persist, 58 cases, about 1 s)
REQ-S13 round trip/determinism/metadata/limits/limits boundary (10,000 ok, 10,001 rejected)/portable export/bundle plan and relocation;
REQ-S14 hostile JSON (size, depth, types, executable keys, dangling refs, URL media paths, bad bundles, tampered manifest, zip-bomb headers);
REQ-X01 crash sweep, disk-full sweep, backups, fallback, newer-format guard, autosave cadence, recovery, migration with fake legacy schema,
StdFileSystem parity (temp directory); settings reject plain secrets.

## Integration API
```
using namespace ss::persist;
StdFileSystem fs;  SystemClock clock;
ShowSession session(fs, clock, "/path/to/My Show.show");            // or MemFileSystem in tests
auto proposal = session.detectRecovery();                           // before open(): optional<RestoreProposal>
  // UI shows proposal->showName/times; accept: session.acceptRestore(*proposal) -> ParsedShow, then start DISARMED
session.open();                                                     // writes the .session lock
auto loaded = session.store().load(&migrationRegistry);             // Result<LoadedShow>: parsed.doc, parsed.extras, notes, migrated
session.noteChanged();                                              // on every show edit (from the show-state writer thread)
session.tickAutosave([&](model::ShowDocument& d, JsonWriteOptions& o){ d = snapshot; o.extras = extras; });   // timer, off the RT path
Status st = session.saveNow(doc, {ExportMode::Local, extras});      // failure: session.dirty() stays true, session.lastError() for the banner
session.closeClean();                                               // after the operator resolved unsaved work
// Portable export:
auto prompts = mediaPrompts(doc);                                   // data for the "include media?" dialog
BundleChoices ch; ch.includeMedia = {7, 9};                         // or ch.includeAllMedia = true
auto plan = planBundle(doc, ch, appVersion);                        // members: manifest.json, show.json (inline content), media/<hash>.<ext> (size, sha256)
  // app: write members with juce::ZipFile; stream media from the library by MediaRef.sha256; plan.warnings go to the UI
// Portable import:
auto v = validateBundleListing(listingFromZip);                     // BEFORE extracting anything; then security::ExtractionBudget while extracting
auto man = parseManifest(manifestText);
checkMemberAgainstManifest(man.value(), path, actualSize, actualSha256);   // per extracted member
auto show = importBundleShow(showJsonText, man.value());            // Result<ParsedShow>, media warnings for excluded files
// Local settings:  loadSettings(fs, path) / saveSettings(fs, path, settings)  (secret *references* only; values live in ISecretStore)
```
Threading: no threads inside; the owner serialises calls per `ShowSession`. File I/O is blocking: call from a worker, not the UI or audio thread.

## Status
- Done and unit-tested: all of the above on Linux (gcc 13 and clang), including crash/disk-full fault injection.
- Partial: `StdFileSystem::writeFile(flush=true)` flushes the C++ stream, which is not an fsync. For real durability supply an app
  `IFileSystem` that fsyncs the file and the directory; the atomic-save logic and its tests do not change. `std::filesystem::rename`
  replaces atomically on POSIX and (MSVC) via MoveFileEx; not exercised on Windows or macOS here.
- Not done: the ZIP container (app layer, `juce::ZipFile`); a real migration (schema major is 1, only the framework exists and is
  tested with a fake legacy schema 0); overlap validation of fixture patches (lighting module's job); enforcing "disarmed" after a
  restore (the proposal states it, the app must obey); a file extension / OS file association for save files (undecided).
- Needs from others: `model::ShowDocument` has no places for unknown-metadata or migration history, so they live in `ShowExtras`
  beside the document; the integrator must carry them from load to save. `lighting` should treat `model::LightingSettings::destination`
  as empty in portable imports.
