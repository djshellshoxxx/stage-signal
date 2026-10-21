# Module Reference: model

**Purpose.** The portable show document. Plain data: scenes, cues, outputs, MIDI mappings, fixture profiles and patches, lighting settings, text messages, media references, journal entries. No I/O and no JSON here. `persist` reads and writes it. `model` only defines the shape and the limits.

**Namespace.** `ss::model`.

## Public header

`core/model/include/ss/model/show_model.hpp`

| Item | Contents |
|---|---|
| Schema | `kSchemaMajor = 1`, `kSchemaMinor = 0` |
| `Limits` | JSON input 8 MiB, 10,000 cues, 1,000 scenes, 256 messages, 512 MIDI mappings, 256 fixtures, 10,000 archive entries, 4,096 text code points, 2 GiB decompressed |
| `ShowDocument` | The whole show: scenes, cues, outputs, seed, mode and flags |
| `SceneDef`, `SceneParams` | A scene's kind, name, tier, pool flag, and look parameters (brightness, speed, intensity, hue, saturation, two effect amounts, prism segments) |
| `CueDef` | Cue scene, duration and time base, trigger wait, transition |
| `OutputDef` | Output label, route, overlays |
| `MidiMapping` | Device, channel, message type, number, range, encoder mode, action |
| `FixtureProfile`, `FixtureChannel`, `FixturePatch` | Channel functions, universe, start address |
| `LightingSettings` | Protocol, destination, universe, rate |
| `TextMessage`, `MediaRef`, `QualityProfileRef` | Text, media and quality references |
| `JournalEntry` | Journal row stored with the show |
| Enums | `SceneKind` (12 generated scenes, plus Image, Gif and Video), `EffectKind`, `EnergyTier`, `TransitionKind`, `TimeBase`, `LightingProtocol`, `MidiMsgType`, `EncoderMode`, `ChannelFunction` |

Scene effect amounts are indexed: 0 is Prism Mirror, 1 is Bloom Pulse.

## Tests

`tests/unit/model`: 1 doctest case. Most coverage of the model comes through `persist` and `show`.

## Status

Done. The shape is used by `show`, `persist`, `lighting` and `diag`.

## Limits and notes

- Tempo and key overrides are not in `ShowDocument`, so a saved show does not keep them yet.
- There is no field for migration history or unknown metadata. `persist` keeps those in `ShowExtras`.
- The lighting group and look model is not defined here. The show module must translate cue lighting references into fixture looks.

## Owner

Core. Changing a field changes the show file format, so see [Show File Format and Migration](Show-File-Format-and-Migration) and the schema rules before you edit.
