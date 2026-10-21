# perf (MOD-14): measurement, gates, adaptive quality, admission

Namespace `ss::perf`, code in `core/perf`, depends on `base` only (nlohmann/json is a private include for the report writer).
No threads, no clocks of its own, no OS counters: callers pass timestamps and raw samples in, so everything is
testable with fake time and fake workloads.

## Scope and requirements

| REQ / item | Where |
|---|---|
| REQ-P01 benchmark reports scenario/config, FPS, frame times/drops, CPU, memory, skipped tests; comparable records | `RunRecorder`, `BenchmarkRunner`, `SuiteReport`, `toJson` |
| REQ-P02 benchmark never touches live outputs, real recorder, lighting or remote broadcast | `SinkDecl`/`sinkAllowed`, `BenchmarkRunner::validate` |
| REQ-P03 one adaptation step at a time with hysteresis, section 7 sequence | `AdaptationController` |
| REQ-P04 locked quality unchanged; warning instead | `AdaptationController::setLocked`, `admitAddedWork` |
| REQ-P06 BASE gates exactly as section 7 | `evaluateBaseGates` |
| REQ-X06 4 h soak criteria | `SoakMonitor` |
| IF-09 `CapabilityStatus` | `admitFeatures().status` |
| Section 6 tiers, section 7 memory sub-budgets | `tiers.hpp` |
| REQ-P05 (queues/memory bounded, capture never waits) | owned by MOD-02; perf supplies the bounded `SampleWindow` and queue-occupancy inputs only |

## Public API (headers under `ss/perf/`)

- `limits.hpp`: every section 7 number (`BaseGateLimits`, `AdaptDefaults`, `kWarmupNs` 15 s, `kRunNs` 60 s, 3 runs).
- `metrics.hpp`: `SampleWindow` (bounded ring, nearest-rank p50/p95/p99, no allocation in `add`), `MetricSet`
  (named values; `available=false` means the counter was missing and is never read as zero), `RunRecorder`
  (`frame`, `presentationDeadline`, `miss(Render|Presentation|Decoder|Encoder)`, `queueOccupancy`, `cpu`, `mem`, `uiAck`,
  `sceneResponse`, `audioToVisualOnset`, `captureOverrun`, `finish`), `normaliseCpu` (per-core % + process CPU time ->
  system avg, busiest core, process % over logical processors).
- `gates.hpp`: `evaluateBaseGates(GateThresholds, GateInput) -> GateReport` (per gate `Pass|Fail|Unavailable`, measured,
  limit, op, unit, req id; verdict `Pass|Fail|Incomplete`).
- `adaptation.hpp`: `AdaptationConfig` (validated ranges), `AdaptationController::update(PressureSample)`,
  `state()` (what the renderer should apply), `setLocked`, `admitAddedWork`.
- `benchmark.hpp`: `scenarios()` BEN-01..06, `IBenchWorkload`, `IBenchTime`/`FakeBenchTime`, `BenchmarkRunner`,
  `BenchEnvironment`, `SuiteReport`, `toJson`.
- `tiers.hpp`: `Tier`, `tierDef`, `classifyTier`, `MemoryBudget`, `MemoryAccountant`.
- `admission.hpp`: `BenchmarkProfile`, `profileFromReport`, `profileToJson/FromJson`, `admitFeatures`, `SoakMonitor`.

## Data flow

1. A workload (app adapter or a fake) is stepped by `BenchmarkRunner`: warm-up 15 s (recorded separately, never gated),
   then 3 x 60 s runs. Each run has its own `RunRecorder`; `finish()` yields a canonical `MetricSet`.
2. `aggregateRuns`: median = lower median of the three runs; worst is direction aware (min for FPS/free memory, max
   for everything else). A metric missing in any run is unavailable in both aggregates.
3. Gates are evaluated on the **worst** run (verdict) and also on the median (reported). Missing counters give
   `Unavailable`, so a verdict is `Pass` only when every evaluated gate was measured and met.
4. `SuiteReport::profileHash()` = SHA-256 of the canonical environment JSON (app/dependency hashes, OS, power plan,
   CPU/GPU/driver, monitor topology, composition sizes, capture format, media hashes, encoder, bitrate). `toJson` is
   deterministic (sorted keys, values rounded to 4 decimals); the caller sets `capturedAtUnixMs`.
5. `profileFromReport` -> persisted with `profileToJson` -> `admitFeatures` compares its hash with the hash of the
   current environment.

## Gates (REQ-P06, section 7)

Mean FPS >= 95% of presentation rate; p95 render <= 80% of period; <= 1% missed presentation deadlines; >= (rate-1)
frames (24 of 25) in 95% of 1 s windows (empty seconds count as failed windows); p95 interval <= 50 ms; p99 <= 100 ms;
no render/interval stall > 200 ms; UI ack p95 <= 100 ms; scene response p95 <= 150 ms; audio->visual onset p95 <= 150 ms
(latency gates only for scenarios with `gateLatency`: BEN-01, BEN-06); working set and private commit steady (median) <= 96 MiB
and peak <= 128 MiB; host working set <= 64 MiB; available physical memory >= 32 MiB; average process CPU <= 65%,
p95 over 1 s windows <= 80%; zero capture overruns (BEN-06 and soak). `G-TARGET-FPS` fails when the configured rate is
below 25 or the run lowered the scene rate by shedding: shedding can never be used to pass the BASE test.

## Adaptation (REQ-P03/P04)

Trigger (any): CPU >= 85% for 5 s; missed deadlines > 5% over 5 s; encoder queue >= 75% for 2 s. At most one step per
5 s, sequence: preview 5 FPS, reduce trails/particles, lower internal render scale, reduce source decode/camera request,
scene FPS 30->25->20->15 (levels below `initialSceneFps`; BASE uses 25, giving 6 steps). Restore one step per 30 s after
CPU < 65%, misses < 1% (trailing 30 s) and queue < 25% have all held for 30 s. Unknown CPU never counts as calm and never
triggers; an absent encoder queue does not block restore. Config is validated (`AdaptationConfig::validate`); invalid
config is rejected with no side effects. Locked: no step in either direction, one `OverloadWarning` per overload
episode, `admitAddedWork` refuses while overloaded. Unlocked: refuses while any shedding is in effect.

## Admission

Per requested feature, in order (earlier admissions consume CPU/memory headroom): hard rejections (tier too low,
required device missing, no encoder, locked+overloaded) cannot be overridden. Soft: no/foreign/stale profile or required
scenario not run -> `Untested` ("untested, run benchmark"); scenario failed or CPU/memory budget exceeded -> `Rejected`.
A soft state is lifted by `AdmissionOverride{featureId, reason}` (non-empty reason required) -> `AdmittedByOverride`.
Headroom = `cpuBudgetPct - cpuMarginPct - baseline CPU` where baseline is BEN-01 (else the cheapest measured scenario).

Tiers: `classifyTier` uses only cores, installed RAM, display count, SSD, GPU-suitable, qualified-encoder flags.
Memory: BASE sub-budgets UI 32, frames 12, DSP 8, source cache 8, transport 4 MiB (+32 MiB runtime = 96 MiB steady cap,
128 MiB peak, 64 MiB host). Higher tiers use the same fractions of the tier's minimum RAM and are flagged `provisional`.

## Limits and failure behaviour

Percentile rings <= 65536 samples; at most 8 named queues; report strings clipped to 256 bytes and lists to 64 entries;
profile JSON <= 64 KiB and <= 16 scenarios; runner config validated (1..10 runs, tick > 0, run <= 1 h). Errors are
`ss::Result`/`Status`: `PermissionDenied` for a refused sink (nothing is prepared or started), `Cancelled` for an
operator abort (workload `finish()` is still called), `InvalidArgument` for bad config, and the workload's own error
from `prepare`. A refused/failed scenario inside `runSuite` is recorded in `skipped` with its reason and the suite continues.

## Tests (`tests/unit/perf`, ~50 cases, no sleeps, fake time)

`REQ-P01` collectors/percentiles/CPU normalisation/runner/aggregation/JSON determinism/profile hash/admission,
`REQ-P02` sink refusal incl. loopback-only discard, `REQ-P03` sequence, triggers, hysteresis, restore, config,
`REQ-P04` lock behaviour, `REQ-P06` every gate boundary and the unlowerable 25 FPS target, `REQ-X06` soak criteria
(simulated 4 h in 1-minute samples), `T-FAULT` stuck encoder queue and operator cancel.

## Status

- Done: everything listed above, as portable logic, tested with fakes.
- Partial: `SoakMonitor` evaluates samples; no 4 h soak was run (no app, no hardware). Section 7 thresholds are
  implemented exactly but are **unqualified** engineering gates: nothing here measures a real 800 MHz / 512 MB machine.
- Not done: real workloads and OS counters (CPU/working set/commit/available memory/GPU timing) belong to the app layer
  behind `IBenchWorkload`/`RunRecorder` inputs; no JSON *reader* for full benchmark reports (only the compact profile);
  tier-specific gate thresholds above BASE and non-BASE memory numbers are provisional (not in the spec).
- Soak growth tolerances (5% or 8 MiB, early-vs-late means) and the stuck-blackout allowance (10 s) are my defaults, tunable in `SoakConfig`.

## Integration API

```cpp
// Live metrics (app feeds counters; one recorder per phase)
perf::RunRecorder rec(25.0); rec.begin(nowNs);
rec.frame(presentedNs, renderNs); rec.presentationDeadline(met); rec.miss(perf::MissKind::Decoder);
rec.queueOccupancy("encoder", occ01); rec.cpu(perf::normaliseCpu(perCorePct, procCpuNsDelta, wallNsDelta, nLogical));
rec.mem(memSample); rec.uiAck(ns); rec.sceneResponse(ns); rec.audioToVisualOnset(ns); rec.captureOverrun();
perf::MetricSet m = rec.finish(nowNs);
// Adaptation: tick ~1 Hz from a control thread (not RT-safe: uses std::deque)
perf::AdaptationConfig cfg; cfg.initialSceneFps = 25;          // BASE
perf::AdaptationController ctl(cfg); ctl.setLocked(show.quality.locked);
auto ev = ctl.update({nowNs, true, cpuPct, dueDelta, missedDelta, true, encoderQueue01});
if (ev.kind != Kind::None) { applyAtSafeBoundary(ctl.state()); journal("adapt", ev.detail); }
if (!ctl.admitAddedWork(&why)) toast(why);
// Benchmark (separate mock output session only)
perf::BenchmarkRunner runner(timeSource, {});
auto suite = runner.runSuite(env, {{"BEN-01", &w1, ""}, {"BEN-03", nullptr, "no camera"}}, seed);
std::string json = perf::toJson(suite.value());              // write to a user-chosen file
std::string profile = perf::profileToJson(perf::profileFromReport(suite.value()));   // persist in settings
// Admission -> IF-09
perf::AdmissionInput in; in.tier = classify.tier; in.currentProfileHash = envHash; in.nowUnixMs = wallMs;
in.profile = &loadedProfile; in.availableDevices = {...}; in.encoderAvailable = b; in.requested = {...};
perf::AdmissionResult r = perf::admitFeatures(in);           // r.status is the CapabilityStatus; r.decisions[i].reasons
// envHash = perf::environmentHash(currentBenchEnvironment) (equals SuiteReport::profileHash() for the same environment).
// Memory
perf::MemoryAccountant mem(perf::memoryBudget(tier)); if (!mem.reserve(perf::MemCategory::Frames, bytes).ok()) refuse();
// Soak: feed one SoakSample per minute, call evaluate() at the end.
```

Needs from others: nothing outside `core/perf`. The app must provide the platform counters and workloads, and map
`AdaptationState` onto the renderer/camera/preview. `router::CapabilityLimits` should be filled from `AdmissionResult`.
