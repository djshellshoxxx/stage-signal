# Module Reference: persist

**Purpose.** Persistence (MOD-13). Show JSON (deterministic), validation before any state is produced, atomic save, autosave and crash recovery, migrations, the portable bundle planner, and local settings. Namespace `ss::persist`. JSON comes from `third_party/nlohmann/json.hpp` and is private to the implementation. No JSON in public headers.

**Depends on.** `base`, `model`, `security`.

## Public headers

| Header | Contents |
|---|---|
| `show_json.hpp` | `ShowExtras`, `ParsedShow`, `JsonWriteOptions`, write and parse of the show document, `PersistLimits` |
| `store.hpp` | `ShowStore` (save, writeRecovery, load, loadFile, removeStaleTemps, backup paths) |
| `session.hpp` | `ShowSession`, `AutosaveController` (30 s debounce), `RestoreProposal`, `LoadSource` |
| `fs.hpp` | `IFileSystem`, `StdFileSystem`, `MemFileSystem` (for tests) |
| `migration.hpp` | `MigrationRegistry`, `MigrationStep`, `MigrationPlan`, `MigrationResult`, `SchemaVersion` |
| `bundle.hpp` | `BundlePlan`, `BundleManifest`, `BundleMember`, `BundleChoices`, `MediaInclusionPrompt` (zip container is the app's job) |
| `settings.hpp` | `LocalSettings`, `DeviceHint`, `SecretReference` (references only, never key values) |

## Behaviour

- Save file: one header line with kind, time, length and SHA-256, then the show JSON. Torn or flipped files fail.
- Atomic save: write temp, flush, rename. Keeps `.bak1` to `.bak3`.
- Autosave: at most once per 30 seconds while dirty, to the recovery file only.
- Validation: size, nesting, array counts, unique ids, references, ranges, UTF-8 text, names, patch fits, IPv4 destinations, canonical media paths, action id registry, and rejection of executable-looking keys. See [Show File Format and Migration](Show-File-Format-and-Migration).
- Migration: back up exact bytes first, apply steps in memory, validate, save only on explicit save.

## Tests

`tests/unit/persist`: 58 doctest cases in `test_bundle.cpp`, `test_misc.cpp`, `test_session.cpp`, `test_show_json.cpp` and `test_store.cpp`, with `fixtures.hpp`. Includes crash and disk-full fault injection through `MemFileSystem`.

## Status (from `docs/specs/persist.md`)

- Done and unit tested on Linux (gcc 13 and clang).
- Partial: `StdFileSystem` flush is not an fsync. Durability needs an app file system that fsyncs the file and the directory. Rename is exercised on POSIX only.
- Not done: the zip container (app layer), a real migration (only a fake schema 0 is tested), overlap checks for patches (lighting's job), enforcing "disarmed" after a restore (the app must obey), and a file association.
- Needs from others: `ShowDocument` has no place for migration history or unknown metadata, so `ShowExtras` carries them. The integrator must keep them from load to save. Lighting should treat the `destination` field as empty in portable imports.
