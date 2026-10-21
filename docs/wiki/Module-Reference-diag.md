# Module Reference: diag

**Purpose.** Diagnostics (MOD-16) and the Show Report. The bounded event journal, the feature sampler and history, log rotation plans, the support bundle composer, the Show Report builder (HTML and CSV), and text helpers. It produces plans and bytes. The app applies the file operations.

**Depends on.** `base`, `model`, `security`.

## Public headers

| Header | Contents |
|---|---|
| `journal.hpp` | `EventJournal` (16 to 100,000 events, sanitised detail, markers, overwritten count, export to model entries), `Event`, `EventKind` |
| `sampler.hpp` | `TempoKeySampler` (at most one sample per second of monotonic time), `FeatureHistory` (ring, default 6 hours), `FeatureSample` |
| `logrotate.hpp` | `planRotation` (5 files of 5 MiB), `planRetention`, `planDeleteAll`, `formatLogLine`, `RotationPlan`, `RotateOp` |
| `support_bundle.hpp` | `SupportBundleComposer` (`addItem`, `setSelected`, `preview`, `compose`), `scanBundleForSecrets`, `BundleCategory`, `ComposedBundle` |
| `show_report.hpp` | `ShowReportBuilder`, `ShowReport` (html, eventsCsv, setlistCsv, summary, setlist, sceneUsage, drops, truncated), `ReportLimits` |
| `text.hpp` | `htmlEscape`, `csvText`, `fixedDecimals`, `formatUtcIso`, `formatDuration`, `keyName` |

## Tests

`tests/unit/diag`: 30 doctest cases in `test_journal.cpp`, `test_logs_bundle.cpp` and `test_show_report.cpp`.

## Status (from `docs/specs/diag.md`)

- Done and unit tested: the portable logic above.
- The Show Report HTML was checked for tag nesting and golden substrings. It has not been viewed in browsers or printed.
- The journal does not detect wall-clock jumps. It records both clocks.
- Not done (app layer): applying rotation to a real folder, zipping the support bundle, collecting the system summary, and the bundle preview UI.
- Key names in the report use sharps only.

## Limits

| Item | Value |
|---|---|
| Journal | 16 to 100,000 events |
| Log rotation | 5 files of 5 MiB, `stagesignal.log` plus `.1` to `.4` |
| Log line | 2000 bytes |
| Bundle | 64 items, 8 MiB each, 32 MiB total |
| Report input | 20,000 events and 100,000 samples |
| Report table rows | 500 per table |
| Report size | HTML 1.5 MiB, CSV 8 MiB |
