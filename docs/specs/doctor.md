# doctor: Show Doctor preflight (DEC-13)

Namespace `ss::doctor`, code in `core/doctor`, depends on `base`, `perf`, `router` (and `model` transitively via router).
It deliberately does **not** depend on `record` or `lighting`: their facts arrive as plain data in the snapshot.

`Doctor::run(const EnvironmentSnapshot&) -> std::vector<Finding>` is a pure function. It has no device, file, network
or clock access, no callbacks and no globals, so no check can transmit anything. The app builds the snapshot from the
other modules, calls `run`, shows the list, and executes a finding's one-click fix through the normal action path
(`ActionId` + value), which keeps the single show-state writer.

## Finding

`Severity {Blocker, Warning, Info}`, stable dotted `id` (per-output ids end in `.<logical output id>`), plain-English
`message`, one-line `why`, optional `Fix{ActionId, value, label}`. Order is deterministic: severity, then id.
Fixes are limited to a safe set (`Doctor::isSafeFixAction`): `DisarmLighting`, `DisableStrobing`, `IdentifyDisplays`.
Nothing the doctor offers can start recording/broadcast/lighting, enable strobing or lift a blackout.
Messages never contain paths or secrets; show-validation text is clipped to 100 characters and at most 3 messages.

## Checks (ids, severity, fix)

| Area | Findings |
|---|---|
| Audio | `audio.permission-denied`, `audio.no-device`, `audio.device-lost`, `audio.format-unsupported`, `audio.not-open` (Blocker; only the root cause is reported); `audio.silent` (Warning, >= 10 s, tunable); `audio.clipping` (Warning) |
| Displays | per output: `display.disconnected.N`, `display.needs-confirmation.N`, `display.suspended.N`, `display.unassigned.N` (Blocker for the Audience output, Warning for extras; fix IdentifyDisplays); `display.disabled.N` (Warning, carries the capability reason); `display.audience-diagnostics.N` (Warning); `display.single-display` (Warning, once); `display.controls-fallback` (Warning); `display.mirror-unavailable` (Info). Unassigned optional extra outputs are silent |
| Disk | `disk.unknown` (Warning), `disk.too-small` (Blocker, planned > free), `disk.low-margin` (Warning, < 1 GiB left after the recording) |
| Lighting (only if configured) | `lighting.patch-conflicts` (Blocker when armed with DisarmLighting fix, else Warning), `lighting.unpatched-fixtures`, `lighting.no-destination` (fix DisarmLighting), `lighting.network-disabled` (fix DisarmLighting), `lighting.armed` (Info, fix DisarmLighting), `lighting.dry-run` (Info) |
| MIDI | `midi.mappings-unavailable` (Warning, counted) |
| Media | `media.missing`, `media.invalid` (Warning) |
| Safety | `safety.strobe-enabled` (Warning, flashing content present and strobing allowed; fix DisableStrobing), `safety.strobe-disabled` (Info), `safety.visual-blackout`, `safety.lighting-blackout` (Warning, no fix: restoring is the operator's call) |
| Benchmark | `benchmark.never-run`, `benchmark.config-changed`, `benchmark.stale` (> 30 days, tunable), `benchmark.failed` (Warning), `benchmark.incomplete` (Info); per feature the show uses: `benchmark.feature-rejected.ID`, `benchmark.feature-untested.ID` (Warning), `benchmark.feature-override.ID` (Info) |
| Broadcast / network | `broadcast.no-secret` (Blocker, only "secret reference present" is known), `broadcast.network-disabled` (Blocker), `network.offline` (Info) |
| Show validation | `show.invalid` (Blocker, errors > 0), `show.warnings` (Warning) |

A healthy environment returns an empty list.

## Snapshot

`EnvironmentSnapshot{ AudioFacts, DisplayFacts{ router::RouteInfo list, displaysConnected, controlsOnFallback }, DiskFacts,
LightingFacts, MidiFacts, MediaFacts, SafetyFacts, BenchmarkFacts{ ageMs, matchesConfig, perf::Verdict, per-feature
perf::AdmissionState + usedByShow + reason }, BroadcastFacts, ValidationFacts, networkDisabled }`, plus
`DoctorOptions{ silenceWarnSeconds, benchmarkMaxAgeMs, diskMarginBytes }`.

## Failure behaviour

`run` cannot fail. Missing information is expressed in the snapshot (`disk.known`, `benchmark.run`) and becomes a
Warning rather than an assumption of health.

## Tests (`tests/unit/doctor`, 19 cases)

One case (or more) per check group, using a real `DisplayRouter` and `perf` types to build realistic snapshots, plus:
healthy rig is empty; every finding is complete and path/secret free; deterministic ordering; a 400-snapshot property
test that every fix is a safe action and never a transmitting/unblackouting one; a compile-time check that `run` takes
only const input; a fix-loop test (apply the offered fixes, blockers disappear).

## Status

- Done: all checks above on plain-data snapshots.
- Not done / out of scope: the app code that fills the snapshot (disk free space, lighting patch conflict counts from the
  lighting module, MIDI port presence, media hash verification) and the UI. "Run Show Doctor" (`ActionId::RunShowDoctor`)
  wiring is the app's. No audio/disk/MIDI device was queried: facts are inputs. Thresholds (10 s silence, 30 days, 1 GiB
  margin) are my defaults, adjustable through `DoctorOptions`.
- Only three fix actions exist; fixes for disk space, missing media or MIDI need operator choices and are intentionally absent.

## Integration API

```cpp
doctor::EnvironmentSnapshot s;
s.audio     = {selected, open, lost, permissionDenied, formatUnsupported, silentSeconds, feature.inputNearFullScale, feature.clipRunLength};
s.displays  = {router.routes(), (unsigned)router.topology().size(), router.controlsOnFallback()};
s.disk      = {freeKnown, freeBytes, recordingPlanned, plannedBytes};
s.lighting  = {configured, armed, dryRun, patchConflictCount, unpatchedFixtureCount, destinationSet};   // from lighting module
s.midi      = {mappingTotal, mappingsWithoutPort};
s.media     = {missingCount, invalidCount};
s.safety    = {showDoc.strobingDisabled, hasFlashRiskMediaOrStrobeFixtures, showSnap.visualBlackout, showSnap.lightingBlackout};
s.benchmark = {haveProfile, ageMs, profileHash == currentHash, verdict, featureAdmissions};              // from perf::admitFeatures
s.broadcast = {configured, secretStore.has(ref)};
s.validation= {errors, warnings, firstMessages};
s.networkDisabled = settings.offlineMode;
auto findings = doctor::Doctor::run(s);              // optionally run(s, doctor::DoctorOptions{...})
if (doctor::Doctor::hasBlockers(findings)) showBlockerBanner();
for (auto& f : findings) {
    // render f.severity, f.message, f.why
    if (f.fix) button(f.fix->label, [&] { submitAction({id(f.fix->action), ActionSource::Ui, kAllOutputs, 0, f.fix->value}); });
}
// ActionId::RunShowDoctor re-runs this. Re-run after every fix (findings are recomputed, nothing is cached).
```

Needs from others: nothing in base/model. ARCHITECTURE.md edge list says `doctor -> perf,router,record,lighting`; this
module uses only `perf` and `router` (plus `model` through router), which is a subset.
