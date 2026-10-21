# router (MOD-05 logic): logical outputs, display roles, hotplug

Namespace `ss::router`, code in `core/router`, depends on `base` and `model`. Pure logic: the app's display manager
feeds in a topology snapshot and applies the resulting routes to windows. No OS calls, no threads, no clocks
(`identify` takes `now` as an argument).

## Scope and requirements

| REQ | Behaviour | Where |
|---|---|---|
| REQ-D01 | Controls + audience roles assigned and restored by matched display **identity** | `assignControls/assignOutput`, `config/applyConfig` |
| REQ-D02 | Third screen as controls mirror OR as a visual output; no second show-state owner | `assignMirror`, `RouteInfo::ownsShowState == false` |
| REQ-D03 | Duplicate vs independent routes; disabled extras explain the measured limit | `CapabilityLimits`, `explain` |
| REQ-D05 | Audience windows never carry diagnostics unless explicitly enabled; assignment survives refresh/focus events | `OverlayPolicy`, `setDiagnostics` |
| REQ-D06 | Lost output = disconnected, never auto-covers controls; reattach needs identity match + safe restore | `setTopology` state machine |
| REQ-D07 (viewport part) | Rotation/resolution change bumps generation + viewport; show content untouched | `ViewportChanged`, `classifyViewport` |
| REQ-D08 (flag) | Per-route final-visual-blackout flag; controls never blacked out | `setFinalBlackout` |
| REQ-A09 | BPM/key (and text/branding) visibility per logical output | `setBpmKeyVisible`, `setOverlay`, `enabledOverlayLayers` |
| IF-06 | `OutputRoute` produced for every logical output | `RouteInfo::route` |
| Section 11 | Ambiguous identical monitors idle until confirmed; operator-triggered identify labels | `confirm`, `identify` |

The renderer-side parts of REQ-D04/D07/D08 (aspect-fit drawing, next-frame blackout) are MOD-06; the router only provides
the data they read.

## Model

- **Physical**: `PhysicalDisplay{identity, connector, width, height, rotationDeg, dpiScale, primary}`. `identity` is the stable
  EDID-derived string (adapter supplies it; serial-less identical models share one identity). `connector` is unique among
  the displays present *now* and is only a transient handle. The OS monitor index never appears anywhere.
- **Logical**: controls (id 1000) and controls mirror (id 1001) are created by the router; visual outputs come from
  `model::OutputDef` (`setOutputs`: ids 1..999, Audience/Duplicate/Independent, duplicate sources validated).
- **Assignment** = logical output -> `(identity, connectorHint)`. `config()/applyConfig()` persist these per machine,
  not in the portable show export. Controls with no assignment follow the OS primary display.

## State machine (per assignment, re-evaluated on every topology/config/capability change)

`Unassigned` -> `Active` | `Disconnected` | `NeedsConfirmation` | `Suspended` | `Disabled`.

1. Resolve: already running on a present display with the same identity -> stays (an identical monitor plugged in later
   does not disturb it); else unique identity match -> candidate; none -> `Disconnected` (bound cleared, nothing drawn
   anywhere else); several identical matches -> `NeedsConfirmation` (idle, previous connector offered as
   `suggestedConnector`, never applied).
2. A candidate becomes `Active` only if it is **safe**: it is not the controls display, not used by another route and
   within capability limits. Otherwise: on the controls display -> `Suspended` (if it was running) or `NeedsConfirmation`;
   used elsewhere -> `NeedsConfirmation`; over limit -> `Disabled` with the caller's reason text.
3. Every (re)activation bumps the route generation; viewport/rotation changes also bump it; blackout and overlay changes
   do not. A returning output inherits the global blackout so it never flashes content.
4. Controls display lost: controls fall back to the OS primary display (else a display not running an output, else the
   first), `controlsOnFallback()` is true, and any output on that display is `Suspended` with its blackout flag forced.
   When the assigned display returns, controls go home and suspended outputs resume.
5. Newly connected, unassigned displays are never adopted automatically (`unassignedDisplays()`).

`setTopology` validates input first (unique non-empty connectors and identities, size 1..16384, rotation in
{0,90,180,270}, <= 16 displays) and changes nothing on error. Events (`RouterEvent`) are journal-ready text without identities.
`tableVersion()` bumps on any change a window must apply; identical refreshes do not change it.
`invariantViolations()` self-checks: no active audience route on the controls display, no two active routes on one
display, no active route on a missing display, controls never blacked out.

## Limits and failures

<= 16 displays, <= 16 logical outputs, strings <= 256 bytes (labels clipped to 64). All commands return `Status`
(`InvalidArgument` with a sanitized message, e.g. "that display hosts the controls"). No exceptions, no allocation
beyond vectors sized by those bounds.

## Tests (`tests/unit/router`, 28 cases)

Named `REQ-D01`, `D02`, `D03`, `D05`, `D06`, `D07`, `D08`, `A09` plus `T-DISPLAY` and `T-FAULT`: simulated 1/2/3 monitor
rigs, restart with reordered OS enumeration, unplug/replug, replacement monitor on the same port, controls lost,
identical monitors (assign, restart, unplug/replug, double assignment), rotation/resolution, capability limits,
overlay policy, blackout flags, identify, input validation, and a 600-step seeded random plug/unplug/assign fuzz that checks
the invariants after every step.

## Status

- Done: all logic above, deterministic and tested against simulated topologies.
- Not done / out of scope: no OS display enumeration, EDID parsing, window creation or mixed-DPI handling (the adapter
  must produce `identity`/`connector`); no real multi-monitor or projector hardware was exercised. DPI scale is carried but
  not used. Rendering of identify labels, borders and blackout is MOD-06/app. REQ-D04 and the drawing half of D07/D08
  are not this module.
- Judgement calls to confirm: an identical-monitor output that is already running is kept when another identical monitor
  appears; after any loss it needs confirmation. Controls auto-fall back so the operator is never locked out.

## Integration API

```cpp
router::DisplayRouter r;
r.setOutputs(showDoc.outputs);                       // on show load; keeps assignments for ids that persist
r.applyConfig(settings.routerConfig);                // per machine, from settings (not the show file)
r.setCapability({maxVisual, maxIndependent, reasonFromAdmission});
std::vector<router::RouterEvent> ev;
r.setTopology(osDisplays, &ev);                      // startup + every WM_DISPLAYCHANGE / hotplug / rotation event
for (auto& e : ev) journal("device", e.text);
// Apply to windows: one window per RouteInfo with route.connected == true, at boundConnector's display;
// all other routes draw nothing (idle). Controls window goes to controlsDisplay().
for (const auto& ri : r.routes()) {
    // ri.route is the IF-06 OutputRoute (generation: drop frames from older generations)
    // ri.finalBlackout: renderer outputs black on that route by the next admitted frame
}
if (r.tableVersion() != lastVersion) reapply();
// Operator actions
r.assignOutput(1, d.identity, d.connector); r.assignControls(...); r.assignMirror(...); r.unassign(id);
r.confirm(id, connector);                            // after the operator picks among identical monitors
r.setBpmKeyVisible(id | kAllOutputs, on);            // ActionId::ToggleOverlayBpmKey backing
r.setOverlay(id, policy); r.setDiagnostics(id, on);  // diagnostics: explicit opt-in only
r.setFinalBlackout(id | kAllOutputs, on);            // from show blackout state (VisualBlackout/Emergency/Restore)
auto s = r.identify(nowNs);                          // ActionId::IdentifyDisplays only; draw s.labels until s.expiresAt
std::string why = r.explain(id);                     // text for disabled/idle outputs
// Persist after any assignment change: settings.routerConfig = r.config();
// Facts for Show Doctor: DisplayFacts{r.routes(), r.topology().size(), r.controlsOnFallback()}.
// Mirror screens: send actions with expectedRevision so show detects stale mirrors; the router never owns state.
```

Needs from others: a display adapter producing stable `identity` strings; `show` calls `setFinalBlackout`;
`perf` admission output feeds `CapabilityLimits`. No changes to base/model needed.
