# Show File Format and Migration

The `persist` module reads and writes show files. Spec: `docs/specs/persist.md`. Current schema: **1.0** (`model::kSchemaMajor = 1`, `kSchemaMinor = 0`).

## Save file on disk

A save file is one header line followed by the show JSON:

```text
STAGESIGNAL-SAVE 1 kind=save|autosave saved=<unix ms> bytes=<n> sha256=<hex>
{"format":"stagesignal-show","schema":{"major":1,"minor":0}, ...}
```

- A file whose length or checksum does not match is rejected. That catches torn writes and flipped bytes.
- Raw JSON with no header is accepted on load only, never written.
- JSON uses sorted keys and is compact, so the same show writes the same bytes.
- 64-bit seeds are decimal strings. Enums are strings such as `"neonGrid"`, never numbers.

## Sidecar files

Next to the save file `<name>` the store may create:

| File | Purpose |
|---|---|
| `<name>.bak1` to `.bak3` | Previous valid saves |
| `<name>.recovery` | Autosave for crash recovery |
| `<name>.session` | Open lock |
| `<name>.tmp` | Atomic-save staging, cleaned up on start |
| `<name>.premigrate-v<N>` | Exact original bytes before a migration |
| `<name>.restored[-n]` | A restored copy |

## Portable bundle

A bundle is a zip with:

- `manifest.json`
- `show.json`
- `media/<sha256>.<ext>`, where ext is one of png, jpg, jpeg, bmp, gif, mp4 or mkv

Media are stored by hash, so a repeated file is stored once. The zip reader that feeds it lives in the app. The portable core checks archive member names and sizes before it extracts anything (see [Security Model](Security-Model)).

## Atomic save

Writes go to a temporary file, are flushed, then renamed over the target. The rename replaces atomically on POSIX. On Windows it uses MoveFileEx. Autosave runs at most once per 30 second window while the document is dirty, and writes only the recovery file.

The portable `flush` is not an fsync. For real durability the app must supply a file system that fsyncs the file and its directory. The test suite injects crashes and disk-full errors to check the logic, not the disk.

## Validation before any state is produced

| Limit | Value |
|---|---|
| Input size | 8 MiB, refused before parsing |
| Nesting depth | 64, refused before parsing |
| Cues | 10,000 |
| Scenes | 1,000 |
| Text messages | 256 |
| MIDI mappings | 512 |
| Fixtures | 256 |
| Journal entries | 50,000 |
| Text | Valid UTF-8, at most 4,096 code points |
| Names | At most 256 bytes |

Semantic checks cover unique ids, every reference resolving (cues to scenes, media, text, profiles and outputs), enum ranges, finite numbers, patch fitting a 512-slot universe, IPv4 lighting destinations, canonical relative media paths (never URLs), and MIDI action ids that exist in the registry.

Keys that look like executable behaviour are rejected anywhere in the file, including `metadata`: `script`, `exec`, `command`, `plugin`, `url`, `macro` and `eval`. Unknown scene kinds, transitions, tiers and action ids are rejected. Other unknown keys are ignored with a warning.

## Schema versions

| Situation | Result |
|---|---|
| Newer major | Refused with `SchemaUnsupported` |
| Newer minor | Loads. Fields it does not know are reported by name only. |
| Older major | Needs a registered migration |

## Migration

`MigrationRegistry` holds pure text-to-text steps (JSON in, JSON out). The flow:

1. Plan the migration.
2. Back up the exact original bytes (flushed, never overwriting an older backup).
3. Apply the steps in memory. Check the schema after each step.
4. Validate the final document.

Loading never rewrites the source file. The migrated show is saved only by an explicit save, which keeps the old file as backup 1. Newer files are never migrated.

Status: the framework is built and tested with a fake legacy schema 0. There is no real migration yet, because there is no older real file.

## Known gaps

- Tempo and key overrides are not part of the show document, so a saved show does not keep them yet. The model needs fields for them.
- Scene effect amounts are stored, but a new effect needs a schema change (see [Adding an Effect](Adding-an-Effect)).
- `ShowDocument` has no field for migration history or unknown metadata. Those live in `ShowExtras` next to the document, and the integrator must carry them from load to save.
- The file association for `.stagesignal` is undecided.
