#include "doctest/doctest.h"
#include <algorithm>
#include "ss/base/rng.hpp"
#include "ss/router/router.hpp"

using namespace ss;
using namespace ss::router;

namespace {
PhysicalDisplay D(const char* identity, const char* connector, int w = 1920, int h = 1080, bool primary = false, int rot = 0) {
    PhysicalDisplay d; d.identity = identity; d.connector = connector; d.width = w; d.height = h; d.primary = primary; d.rotationDeg = rot;
    return d;
}
model::OutputDef out(OutputId id, RouteKind k = RouteKind::Audience, const char* label = "Main", OutputId dupOf = 0) {
    model::OutputDef o; o.id = id; o.kind = k; o.label = label; o.duplicateOf = dupOf;
    return o;
}
bool hasEvent(const std::vector<RouterEvent>& ev, RouterEvent::Type t, OutputId id = 0) {
    for (const auto& e : ev) if (e.type == t && (id == 0 || e.output == id)) return true;
    return false;
}
RouteInfo R(const DisplayRouter& r, OutputId id) { auto x = r.find(id); REQUIRE(x.has_value()); return *x; }

// Standard rig: controls on laptop panel (primary), audience on projector.
struct Rig {
    DisplayRouter r;
    Rig() {
        REQUIRE(r.setOutputs({out(1)}).ok());
        REQUIRE(r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true), D("PROJ-A", "HDMI-1", 1024, 768)}).ok());
        REQUIRE(r.assignControls("LAPTOP-PANEL", "eDP-1").ok());
        REQUIRE(r.assignOutput(1, "PROJ-A", "HDMI-1").ok());
    }
};
}  // namespace

TEST_CASE("REQ-D01: primary controls and a second audience output are assigned by stable identity") {
    Rig g;
    RouteInfo c = R(g.r, kControlsOutputId), a = R(g.r, 1);
    CHECK(c.status == RouteStatus::Active);
    CHECK(c.route.kind == RouteKind::Controls);
    CHECK(c.route.displayIdentity == "LAPTOP-PANEL");
    CHECK(a.status == RouteStatus::Active);
    CHECK(a.route.connected);
    CHECK(a.route.displayIdentity == "PROJ-A");
    CHECK(a.route.viewportW == 1024); CHECK(a.route.viewportH == 768);
    CHECK(a.route.kind == RouteKind::Audience);
    CHECK(g.r.invariantViolations().empty());
}

TEST_CASE("REQ-D01: restart preserves roles on matched identities even when OS order and connectors change") {
    RouterConfig saved;
    { Rig g; saved = g.r.config(); }
    CHECK(saved.assignments.size() == 2);
    DisplayRouter r2;
    REQUIRE(r2.setOutputs({out(1)}).ok());
    REQUIRE(r2.applyConfig(saved).ok());
    // The OS now enumerates the projector first, on a different connector, and the laptop is no longer "primary" first.
    REQUIRE(r2.setTopology({D("PROJ-A", "DP-7", 1024, 768), D("LAPTOP-PANEL", "eDP-1", 1366, 768, true)}).ok());
    CHECK(R(r2, kControlsOutputId).status == RouteStatus::Active);
    CHECK(R(r2, kControlsOutputId).boundConnector == "eDP-1");
    CHECK(R(r2, 1).status == RouteStatus::Active);
    CHECK(R(r2, 1).boundConnector == "DP-7");
    CHECK(r2.invariantViolations().empty());
    // Persisted config holds identities, never indexes.
    for (const auto& a : saved.assignments) CHECK(a.identity.find_first_of("0123456789") != 0);
}

TEST_CASE("REQ-D01: with no saved controls assignment the controls follow the OS primary display") {
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(1)}).ok());
    REQUIRE(r.setTopology({D("A", "c1"), D("B", "c2", 1920, 1080, true)}).ok());
    CHECK(R(r, kControlsOutputId).boundConnector == "c2");
    CHECK(R(r, 1).status == RouteStatus::Unassigned);
    CHECK(r.unassignedDisplays().size() == 1);
}

TEST_CASE("REQ-D03: single monitor leaves the audience output off and explains why") {
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(1), out(2, RouteKind::Duplicate, "Dup", 1)}).ok());
    REQUIRE(r.setTopology({D("ONLY", "eDP-1", 1280, 800, true)}).ok());
    CHECK(R(r, kControlsOutputId).status == RouteStatus::Active);
    CHECK(R(r, 1).status == RouteStatus::Unassigned);
    CHECK_FALSE(R(r, 1).route.connected);
    CHECK(r.explain(1).find("second display") != std::string::npos);
    // nothing can be assigned onto the controls display
    CHECK_FALSE(r.assignOutput(1, "ONLY", "eDP-1").ok());
    CHECK(r.invariantViolations().empty());
}

TEST_CASE("REQ-D02: third screen as controls mirror OR as visuals, never a second show-state owner") {
    for (int asVisual = 0; asVisual < 2; ++asVisual) {
        DisplayRouter r;
        REQUIRE(r.setOutputs({out(1), out(2, RouteKind::Independent, "Side")}).ok());
        REQUIRE(r.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("PROJ", "HDMI-1"), D("THIRD", "DP-1", 1280, 1024)}).ok());
        REQUIRE(r.assignControls("LAP", "eDP-1").ok());
        REQUIRE(r.assignOutput(1, "PROJ", "HDMI-1").ok());
        if (asVisual) REQUIRE(r.assignOutput(2, "THIRD", "DP-1").ok());
        else REQUIRE(r.assignMirror("THIRD", "DP-1").ok());
        auto routes = r.routes();
        int active = 0;
        for (const auto& x : routes) {
            CHECK_FALSE(x.ownsShowState);                      // the show module is the only writer
            if (x.status == RouteStatus::Active) ++active;
        }
        CHECK(active == 4 - 1 + 0);                            // controls, audience, and the third screen (mirror slot or output 2)
        if (asVisual) { CHECK(R(r, 2).route.kind == RouteKind::Independent); CHECK(R(r, kMirrorOutputId).status == RouteStatus::Unassigned); }
        else { CHECK(R(r, kMirrorOutputId).isMirror); CHECK(R(r, kMirrorOutputId).status == RouteStatus::Active); CHECK(R(r, 2).status == RouteStatus::Unassigned); }
        CHECK(r.invariantViolations().empty());
        CHECK(r.unassignedDisplays().empty());
    }
}

TEST_CASE("REQ-D03: duplicate and independent routes keep IF-06 semantics") {
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(1), out(2, RouteKind::Duplicate, "Dup", 1), out(3, RouteKind::Independent, "Ind")}).ok());
    REQUIRE(r.setTopology({D("L", "eDP-1", 1366, 768, true), D("P1", "h1"), D("P2", "h2"), D("P3", "h3")}).ok());
    REQUIRE(r.assignControls("L", "eDP-1").ok());
    REQUIRE(r.assignOutput(1, "P1", "h1").ok());
    REQUIRE(r.assignOutput(2, "P2", "h2").ok());
    REQUIRE(r.assignOutput(3, "P3", "h3").ok());
    CHECK(R(r, 2).route.kind == RouteKind::Duplicate);
    CHECK(R(r, 2).route.duplicateOf == 1);                 // shares the composition of output 1
    CHECK(R(r, 3).route.kind == RouteKind::Independent);
    CHECK(R(r, 3).route.duplicateOf == 0);
    // invalid duplicate sources are refused
    CHECK_FALSE(r.setOutputs({out(1), out(2, RouteKind::Duplicate, "D", 9)}).ok());
    CHECK_FALSE(r.setOutputs({out(1, RouteKind::Duplicate, "D", 1)}).ok());
    CHECK_FALSE(r.setOutputs({out(1), out(2, RouteKind::Duplicate, "D", 3), out(3, RouteKind::Duplicate, "E", 1)}).ok());
    CHECK_FALSE(r.setOutputs({out(1), out(1)}).ok());
    CHECK_FALSE(r.setOutputs({out(0)}).ok());
    CHECK_FALSE(r.setOutputs({out(kControlsOutputId)}).ok());
    CHECK_FALSE(r.setOutputs({out(1, RouteKind::Controls)}).ok());
}

TEST_CASE("REQ-D03: capability limits disable extra outputs and explain the measured limit") {
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(1), out(2, RouteKind::Duplicate, "Dup", 1), out(3, RouteKind::Independent, "Ind")}).ok());
    REQUIRE(r.setTopology({D("L", "eDP-1", 1366, 768, true), D("P1", "h1"), D("P2", "h2"), D("P3", "h3")}).ok());
    REQUIRE(r.assignControls("L", "eDP-1").ok());
    REQUIRE(r.assignOutput(1, "P1", "h1").ok());
    REQUIRE(r.assignOutput(2, "P2", "h2").ok());
    REQUIRE(r.assignOutput(3, "P3", "h3").ok());
    CapabilityLimits cap; cap.maxVisualOutputs = 1; cap.reason = "BEN-02 measured capacity for 1 output at 25 FPS on this machine";
    r.setCapability(cap);
    CHECK(R(r, 1).status == RouteStatus::Active);
    CHECK(R(r, 2).status == RouteStatus::Disabled);
    CHECK(R(r, 3).status == RouteStatus::Disabled);
    CHECK_FALSE(R(r, 2).route.connected);
    CHECK(r.explain(2).find("BEN-02 measured capacity") != std::string::npos);
    CHECK(r.invariantViolations().empty());
    // independent compositions have their own limit
    cap.maxVisualOutputs = 3; cap.maxIndependentOutputs = 0; cap.reason = "only one composition measured";
    r.setCapability(cap);
    CHECK(R(r, 2).status == RouteStatus::Active);
    CHECK(R(r, 3).status == RouteStatus::Disabled);
    CHECK(r.explain(3).find("only one composition") != std::string::npos);
    cap.maxIndependentOutputs = 4;
    r.setCapability(cap);
    CHECK(R(r, 3).status == RouteStatus::Active);
    // default reason text when the caller gives none
    CapabilityLimits none; none.maxVisualOutputs = 0;
    r.setCapability(none);
    CHECK(R(r, 1).status == RouteStatus::Disabled);
    CHECK_FALSE(r.explain(1).empty());
}

TEST_CASE("REQ-D06: a lost output is marked disconnected and the controls are never moved onto it or covered") {
    Rig g;
    std::vector<RouterEvent> ev;
    const Generation genBefore = R(g.r, 1).route.generation;
    REQUIRE(g.r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true)}, &ev).ok());
    RouteInfo a = R(g.r, 1);
    CHECK(a.status == RouteStatus::Disconnected);
    CHECK_FALSE(a.route.connected);
    CHECK(a.idle);
    CHECK(a.boundConnector.empty());
    CHECK(hasEvent(ev, RouterEvent::Type::Disconnected, 1));
    CHECK(hasEvent(ev, RouterEvent::Type::DisplayRemoved));
    RouteInfo c = R(g.r, kControlsOutputId);
    CHECK(c.status == RouteStatus::Active);
    CHECK(c.boundConnector == "eDP-1");
    CHECK_FALSE(g.r.controlsOnFallback());
    CHECK(g.r.invariantViolations().empty());
    CHECK(g.r.explain(1).find("controls were not moved") != std::string::npos);
    CHECK(a.route.generation >= genBefore);
}

TEST_CASE("REQ-D06: reattach requires an identity match; a different monitor on the same port is not adopted") {
    Rig g;
    REQUIRE(g.r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true)}).ok());
    // a different projector is plugged into the same connector
    REQUIRE(g.r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true), D("OTHER-TV", "HDMI-1", 1920, 1080)}).ok());
    CHECK(R(g.r, 1).status == RouteStatus::Disconnected);
    CHECK(g.r.unassignedDisplays().size() == 1);
    // the original comes back (possibly on another connector): restored automatically because it is safe
    std::vector<RouterEvent> ev;
    const Generation gen = R(g.r, 1).route.generation;
    REQUIRE(g.r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true), D("OTHER-TV", "HDMI-1", 1920, 1080), D("PROJ-A", "DP-3", 1024, 768)}, &ev).ok());
    CHECK(R(g.r, 1).status == RouteStatus::Active);
    CHECK(R(g.r, 1).boundConnector == "DP-3");
    CHECK(R(g.r, 1).route.generation > gen);               // stale frames for the old binding are discarded
    CHECK(hasEvent(ev, RouterEvent::Type::Reattached, 1));
    CHECK(g.r.invariantViolations().empty());
}

TEST_CASE("REQ-D06: a returning output never shows content during a blackout") {
    Rig g;
    g.r.setFinalBlackout(kAllOutputs, true);
    REQUIRE(g.r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true)}).ok());
    g.r.setFinalBlackout(kAllOutputs, false);              // blackout released while the output is away
    g.r.setFinalBlackout(kAllOutputs, true);               // and engaged again
    REQUIRE(g.r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true), D("PROJ-A", "HDMI-1", 1024, 768)}).ok());
    CHECK(R(g.r, 1).status == RouteStatus::Active);
    CHECK(R(g.r, 1).finalBlackout);
    CHECK_FALSE(R(g.r, kControlsOutputId).finalBlackout);
}

TEST_CASE("REQ-D06: losing the controls display keeps the controls reachable and pauses any output on the fallback") {
    Rig g;
    std::vector<RouterEvent> ev;
    // The laptop panel disappears (lid closed); only the projector remains.
    REQUIRE(g.r.setTopology({D("PROJ-A", "HDMI-1", 1024, 768, true)}, &ev).ok());
    CHECK(g.r.controlsOnFallback());
    CHECK(R(g.r, kControlsOutputId).boundConnector == "HDMI-1");
    CHECK(hasEvent(ev, RouterEvent::Type::ControlsFallback));
    RouteInfo a = R(g.r, 1);
    CHECK(a.status == RouteStatus::Suspended);
    CHECK_FALSE(a.route.connected);
    CHECK(a.finalBlackout);                                 // no audience content is drawn over the controls
    CHECK(g.r.invariantViolations().empty());
    PhysicalDisplay cd;
    REQUIRE(g.r.controlsDisplay(&cd));
    CHECK(cd.connector == "HDMI-1");
    // The panel returns: controls go home and the audience output resumes.
    ev.clear();
    REQUIRE(g.r.setTopology({D("PROJ-A", "HDMI-1", 1024, 768), D("LAPTOP-PANEL", "eDP-1", 1366, 768, true)}, &ev).ok());
    CHECK_FALSE(g.r.controlsOnFallback());
    CHECK(R(g.r, kControlsOutputId).boundConnector == "eDP-1");
    CHECK(R(g.r, 1).status == RouteStatus::Active);
    CHECK(hasEvent(ev, RouterEvent::Type::ControlsRestored));
    CHECK(hasEvent(ev, RouterEvent::Type::Resumed, 1));
    CHECK(g.r.invariantViolations().empty());
}

TEST_CASE("REQ-D06: with three monitors the controls fall back to a display that is not running an output") {
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(1)}).ok());
    REQUIRE(r.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("PROJ", "h1"), D("SPARE", "h2", 800, 600)}).ok());
    REQUIRE(r.assignControls("LAP", "eDP-1").ok());
    REQUIRE(r.assignOutput(1, "PROJ", "h1").ok());
    REQUIRE(r.setTopology({D("PROJ", "h1"), D("SPARE", "h2", 800, 600)}).ok());      // no primary flag: choose the idle one
    CHECK(r.controlsOnFallback());
    CHECK(R(r, kControlsOutputId).boundConnector == "h2");
    CHECK(R(r, 1).status == RouteStatus::Active);
    CHECK(r.invariantViolations().empty());
}

TEST_CASE("REQ-D06: no displays at all reports disconnected without crashing") {
    Rig g;
    REQUIRE(g.r.setTopology({}).ok());
    CHECK(R(g.r, kControlsOutputId).status == RouteStatus::Disconnected);
    CHECK(R(g.r, 1).status == RouteStatus::Disconnected);
    PhysicalDisplay cd;
    CHECK_FALSE(g.r.controlsDisplay(&cd));
    CHECK(g.r.invariantViolations().empty());
}

TEST_CASE("REQ-D06: a saved audience display that is the controls display after a change is not restored onto it") {
    // Saved: audience on PROJ-A. Next launch only PROJ-A is connected and it is primary: controls must win.
    RouterConfig saved;
    { Rig g; saved = g.r.config(); }
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(1)}).ok());
    REQUIRE(r.applyConfig(saved).ok());
    REQUIRE(r.setTopology({D("PROJ-A", "HDMI-1", 1024, 768, true)}).ok());
    CHECK(r.controlsOnFallback());
    CHECK(R(r, 1).status != RouteStatus::Active);
    CHECK(R(r, 1).idle);
    CHECK(r.invariantViolations().empty());
}

TEST_CASE("REQ-D06: identical monitors stay idle until the operator confirms which one") {
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(1)}).ok());
    REQUIRE(r.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("TV-MODEL", "h1"), D("TV-MODEL", "h2")}).ok());
    REQUIRE(r.assignControls("LAP", "eDP-1").ok());
    // explicit choice of one of the two identical monitors is accepted
    REQUIRE(r.assignOutput(1, "TV-MODEL", "h2").ok());
    CHECK(R(r, 1).status == RouteStatus::Active);
    CHECK(R(r, 1).boundConnector == "h2");
    RouterConfig saved = r.config();

    // restart: both identical monitors present again -> ambiguous, idle, nothing drawn
    DisplayRouter r2;
    REQUIRE(r2.setOutputs({out(1)}).ok());
    REQUIRE(r2.applyConfig(saved).ok());
    REQUIRE(r2.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("TV-MODEL", "h1"), D("TV-MODEL", "h2")}).ok());
    RouteInfo a = R(r2, 1);
    CHECK(a.status == RouteStatus::NeedsConfirmation);
    CHECK(a.idle);
    CHECK_FALSE(a.route.connected);
    CHECK(a.boundConnector.empty());
    CHECK(a.suggestedConnector == "h2");                    // previous choice offered, not applied
    CHECK(r2.explain(1).find("identical") != std::string::npos);
    CHECK(r2.invariantViolations().empty());
    // confirming a display that does not match is refused; the right one activates
    CHECK_FALSE(r2.confirm(1, "eDP-1").ok());
    CHECK_FALSE(r2.confirm(1, "nope").ok());
    REQUIRE(r2.confirm(1, "h1").ok());
    CHECK(R(r2, 1).status == RouteStatus::Active);
    CHECK(R(r2, 1).boundConnector == "h1");
    CHECK(R(r2, 1).route.generation > a.route.generation);
    CHECK(r2.invariantViolations().empty());
}

TEST_CASE("REQ-D06: unplug and replug of one identical monitor requires reassignment, a running one is left alone") {
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(1)}).ok());
    REQUIRE(r.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("TV", "h1")}).ok());
    REQUIRE(r.assignControls("LAP", "eDP-1").ok());
    REQUIRE(r.assignOutput(1, "TV", "h1").ok());
    // a second identical TV is plugged in while the first is running: the running unit is untouched
    REQUIRE(r.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("TV", "h1"), D("TV", "h2")}).ok());
    CHECK(R(r, 1).status == RouteStatus::Active);
    CHECK(R(r, 1).boundConnector == "h1");
    // the running one is unplugged and a TV comes back on the other connector: cannot tell which, so idle
    REQUIRE(r.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("TV", "h2")}).ok());
    CHECK(R(r, 1).status == RouteStatus::Active);          // only one TV present => unique again, safe to restore
    REQUIRE(r.setTopology({D("LAP", "eDP-1", 1366, 768, true)}).ok());
    CHECK(R(r, 1).status == RouteStatus::Disconnected);
    REQUIRE(r.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("TV", "h3"), D("TV", "h4")}).ok());
    CHECK(R(r, 1).status == RouteStatus::NeedsConfirmation);
    CHECK(R(r, 1).idle);
    CHECK(r.unassignedDisplays().size() == 2);
    REQUIRE(r.confirm(1, "h4").ok());
    CHECK(R(r, 1).boundConnector == "h4");
    CHECK(r.invariantViolations().empty());
}

TEST_CASE("REQ-D06: identical monitors cannot be assigned to two outputs at once") {
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(1), out(2, RouteKind::Independent, "B")}).ok());
    REQUIRE(r.setTopology({D("LAP", "eDP-1", 1366, 768, true), D("TV", "h1"), D("TV", "h2")}).ok());
    REQUIRE(r.assignOutput(1, "TV", "h1").ok());
    CHECK_FALSE(r.assignOutput(2, "TV", "h1").ok());        // already hosts output 1
    REQUIRE(r.assignOutput(2, "TV", "h2").ok());
    CHECK(R(r, 1).boundConnector == "h1");
    CHECK(R(r, 2).boundConnector == "h2");
    CHECK(r.invariantViolations().empty());
}

TEST_CASE("REQ-D07: rotation and resolution changes update the viewport and generation, not the assignment") {
    Rig g;
    const RouteInfo before = R(g.r, 1);
    std::vector<RouterEvent> ev;
    REQUIRE(g.r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true), D("PROJ-A", "HDMI-1", 768, 1024, false, 90)}, &ev).ok());
    RouteInfo rot = R(g.r, 1);
    CHECK(rot.status == RouteStatus::Active);
    CHECK(rot.route.viewportW == 768); CHECK(rot.route.viewportH == 1024); CHECK(rot.route.rotationDeg == 90);
    CHECK(rot.route.generation > before.route.generation);
    CHECK(rot.route.displayIdentity == before.route.displayIdentity);
    CHECK(hasEvent(ev, RouterEvent::Type::ViewportChanged, 1));
    // same topology again: nothing changes (no generation churn)
    const std::uint64_t ver = g.r.tableVersion();
    REQUIRE(g.r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true), D("PROJ-A", "HDMI-1", 768, 1024, false, 90)}).ok());
    CHECK(R(g.r, 1).route.generation == rot.route.generation);
    CHECK(g.r.tableVersion() == ver);
    REQUIRE(g.r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true), D("PROJ-A", "HDMI-1", 1280, 800)}).ok());
    CHECK(R(g.r, 1).route.viewportW == 1280);
    CHECK(R(g.r, 1).route.generation > rot.route.generation);
    CHECK(classifyViewport(800, 600).kind == ViewportClass::Kind::Ratio4x3);
    CHECK(classifyViewport(1920, 1080).kind == ViewportClass::Kind::Ratio16x9);
    CHECK(classifyViewport(1280, 800).kind == ViewportClass::Kind::Ratio16x10);
    CHECK(classifyViewport(1080, 1920).kind == ViewportClass::Kind::Portrait);
    CHECK(classifyViewport(2000, 700).kind == ViewportClass::Kind::Other);
    CHECK(classifyViewport(0, 10).kind == ViewportClass::Kind::Other);
}

TEST_CASE("REQ-D05: audience outputs carry no diagnostics unless explicitly enabled") {
    Rig g;
    CHECK_FALSE(R(g.r, 1).route.showDiagnostics);
    CHECK(R(g.r, kControlsOutputId).route.showDiagnostics);
    CHECK(enabledOverlayLayers(R(g.r, 1).route) == std::vector<std::string>{"text"});
    REQUIRE(g.r.setDiagnostics(1, true).ok());
    CHECK(R(g.r, 1).route.showDiagnostics);
    CHECK(enabledOverlayLayers(R(g.r, 1).route) == (std::vector<std::string>{"text", "diagnostics"}));
    REQUIRE(g.r.setDiagnostics(1, false).ok());
    CHECK_FALSE(R(g.r, 1).route.showDiagnostics);
    CHECK_FALSE(g.r.setDiagnostics(kControlsOutputId, false).ok());
    // reconnecting or rotating never turns diagnostics on
    REQUIRE(g.r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true)}).ok());
    REQUIRE(g.r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true), D("PROJ-A", "HDMI-1")}).ok());
    CHECK_FALSE(R(g.r, 1).route.showDiagnostics);
}

TEST_CASE("REQ-D05: assignments are by identity so focus or window changes cannot move a route") {
    Rig g;
    const std::uint64_t v = g.r.tableVersion();
    const auto before = g.r.routes();
    for (int i = 0; i < 5; ++i) REQUIRE(g.r.setTopology(g.r.topology()).ok());   // repeated refreshes (e.g. focus/activation events)
    CHECK(g.r.tableVersion() == v);
    const auto after = g.r.routes();
    REQUIRE(before.size() == after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK(before[i].boundConnector == after[i].boundConnector);
        CHECK(before[i].route.generation == after[i].route.generation);
    }
}

TEST_CASE("REQ-A09: BPM/key visibility is configured per logical output and reflected in the route") {
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(1), out(2, RouteKind::Independent, "Side")}).ok());
    REQUIRE(r.setTopology({D("L", "eDP-1", 1366, 768, true), D("P1", "h1"), D("P2", "h2")}).ok());
    REQUIRE(r.assignOutput(1, "P1", "h1").ok());
    REQUIRE(r.assignOutput(2, "P2", "h2").ok());
    CHECK(enabledOverlayLayers(R(r, 1).route) == std::vector<std::string>{"text"});
    REQUIRE(r.setBpmKeyVisible(1, true).ok());
    CHECK(enabledOverlayLayers(R(r, 1).route) == (std::vector<std::string>{"text", "bpm-key"}));
    CHECK(enabledOverlayLayers(R(r, 2).route) == std::vector<std::string>{"text"});     // other output untouched
    REQUIRE(r.setBpmKeyVisible(kAllOutputs, false).ok());
    CHECK_FALSE(R(r, 1).route.showBpmKey);
    OverlayPolicy p; p.showBpmKey = true; p.showText = false; p.showBranding = true;
    REQUIRE(r.setOverlay(2, p).ok());
    CHECK(enabledOverlayLayers(R(r, 2).route) == (std::vector<std::string>{"bpm-key", "branding"}));
    CHECK_FALSE(r.setBpmKeyVisible(kControlsOutputId, false).ok());
    CHECK_FALSE(r.setOverlay(99, p).ok());
    // the policy survives a disconnect/reconnect of the output
    REQUIRE(r.setTopology({D("L", "eDP-1", 1366, 768, true), D("P1", "h1")}).ok());
    REQUIRE(r.setTopology({D("L", "eDP-1", 1366, 768, true), D("P1", "h1"), D("P2", "h2")}).ok());
    CHECK(enabledOverlayLayers(R(r, 2).route) == (std::vector<std::string>{"bpm-key", "branding"}));
}

TEST_CASE("REQ-A09: overlay settings from the show document seed the routes") {
    DisplayRouter r;
    model::OutputDef o = out(1); o.showBpmKey = true; o.showText = false; o.showBranding = true;
    REQUIRE(r.setOutputs({o}).ok());
    CHECK(enabledOverlayLayers(R(r, 1).route) == (std::vector<std::string>{"bpm-key", "branding"}));
    // re-loading the same outputs keeps assignments
    REQUIRE(r.setTopology({D("L", "eDP-1", 1366, 768, true), D("P1", "h1")}).ok());
    REQUIRE(r.assignOutput(1, "P1", "h1").ok());
    REQUIRE(r.setOutputs({o}).ok());
    CHECK(R(r, 1).status == RouteStatus::Active);
}

TEST_CASE("REQ-D08: final-visual-blackout is a per-route flag; controls stay visible") {
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(1), out(2, RouteKind::Independent, "Side")}).ok());
    REQUIRE(r.setTopology({D("L", "eDP-1", 1366, 768, true), D("P1", "h1"), D("P2", "h2")}).ok());
    REQUIRE(r.assignOutput(1, "P1", "h1").ok());
    REQUIRE(r.assignOutput(2, "P2", "h2").ok());
    const std::uint64_t v0 = r.tableVersion();
    r.setFinalBlackout(1, true);
    CHECK(R(r, 1).finalBlackout);
    CHECK_FALSE(R(r, 2).finalBlackout);
    CHECK(r.tableVersion() > v0);                           // renderer sees the change on its next table read
    r.setFinalBlackout(kAllOutputs, true);
    CHECK(R(r, 2).finalBlackout);
    CHECK_FALSE(R(r, kControlsOutputId).finalBlackout);
    CHECK_FALSE(R(r, kMirrorOutputId).finalBlackout);
    r.setFinalBlackout(kControlsOutputId, true);            // controls cannot be blacked out
    CHECK_FALSE(R(r, kControlsOutputId).finalBlackout);
    CHECK(r.globalBlackout());
    r.setFinalBlackout(kAllOutputs, false);
    CHECK_FALSE(R(r, 1).finalBlackout);
    CHECK_FALSE(r.globalBlackout());
    // blackout does not change the route generation (frames already in flight stay valid)
    const Generation g = R(r, 1).route.generation;
    r.setFinalBlackout(1, true); r.setFinalBlackout(1, false);
    CHECK(R(r, 1).route.generation == g);
    CHECK(r.invariantViolations().empty());
}

TEST_CASE("REQ-D01: identify displays is operator-triggered, labelled by ordinal and role, and expires") {
    Rig g;
    IdentifySession s = g.r.identify(1000, 5 * kNsPerSec);
    CHECK(s.active);
    REQUIRE(s.labels.size() == 2);
    CHECK(s.labels[0].ordinal == 1); CHECK(s.labels[1].ordinal == 2);
    CHECK(s.labels[0].connector == "HDMI-1");              // stable ordering by connector
    CHECK(s.labels[0].subtitle.find("Audience: Main") == 0);
    CHECK(s.labels[0].subtitle.find("1024x768") != std::string::npos);
    CHECK(s.labels[1].subtitle.find("Controls") == 0);
    for (const auto& l : s.labels) { CHECK(l.borderGrid); CHECK(l.title.find("Display ") == 0); CHECK(l.subtitle.find("PROJ") == std::string::npos); }
    CHECK(s.activeAt(1000 + 4 * kNsPerSec));
    CHECK_FALSE(s.activeAt(1000 + 5 * kNsPerSec));
    IdentifySession clamped = g.r.identify(0, 1);
    CHECK(clamped.expiresAt >= 2 * kNsPerSec);
    DisplayRouter empty;
    CHECK_FALSE(empty.identify(0).active);
    // unassigned display is labelled as such
    REQUIRE(g.r.unassign(1).ok());
    CHECK(g.r.identify(0).labels[0].subtitle.find("Not assigned") == 0);
}

TEST_CASE("REQ-D01: assignment rules refuse unsafe or impossible choices") {
    Rig g;
    CHECK_FALSE(g.r.assignOutput(1, "PROJ-A", "eDP-1").ok());           // identity does not match that connector
    CHECK_FALSE(g.r.assignOutput(1, "LAPTOP-PANEL", "eDP-1").ok());     // controls display
    CHECK_FALSE(g.r.assignOutput(1, "", "HDMI-1").ok());
    CHECK_FALSE(g.r.assignOutput(77, "PROJ-A", "HDMI-1").ok());
    CHECK_FALSE(g.r.assignOutput(kControlsOutputId, "PROJ-A", "HDMI-1").ok());
    CHECK_FALSE(g.r.assignControls("PROJ-A", "HDMI-1").ok());           // would sit on the active audience display
    CHECK_FALSE(g.r.assignMirror("LAPTOP-PANEL", "eDP-1").ok());        // mirror must differ from the controls
    CHECK_FALSE(g.r.confirm(1, "HDMI-1").ok());                         // nothing to confirm
    CHECK_FALSE(g.r.unassign(77).ok());
    CHECK(g.r.unassign(1).ok());
    CHECK(R(g.r, 1).status == RouteStatus::Unassigned);
    CHECK(R(g.r, 1).route.displayIdentity.empty());
    CHECK(g.r.invariantViolations().empty());
}

TEST_CASE("REQ-D01: topology input is validated") {
    DisplayRouter r;
    CHECK_FALSE(r.setTopology({D("A", "c"), D("B", "c")}).ok());        // duplicate connector
    CHECK_FALSE(r.setTopology({D("", "c")}).ok());
    CHECK_FALSE(r.setTopology({D("A", "")}).ok());
    CHECK_FALSE(r.setTopology({D("A", "c", 0, 600)}).ok());
    CHECK_FALSE(r.setTopology({D("A", "c", 800, 600, false, 45)}).ok());
    Topology many;
    for (int i = 0; i < 17; ++i) many.push_back(D("X", ("c" + std::to_string(i)).c_str()));
    CHECK_FALSE(r.setTopology(many).ok());
    RouterConfig big; big.assignments.push_back({1, std::string(300, 'x'), ""});
    CHECK_FALSE(r.applyConfig(big).ok());
    CHECK(r.topology().empty());                                         // a rejected topology changes nothing
}

TEST_CASE("T-DISPLAY: persisted config never contains monitor indexes and ignores outputs the show no longer has") {
    Rig g;
    RouterConfig c = g.r.config();
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(5)}).ok());
    REQUIRE(r.applyConfig(c).ok());                                      // assignment for output 1 is dropped, controls kept
    REQUIRE(r.setTopology({D("LAPTOP-PANEL", "eDP-1", 1366, 768, true), D("PROJ-A", "HDMI-1")}).ok());
    CHECK(R(r, 5).status == RouteStatus::Unassigned);
    CHECK_FALSE(r.find(1).has_value());
    CHECK(R(r, kControlsOutputId).status == RouteStatus::Active);
}

TEST_CASE("T-FAULT: random plug/unplug/rotate sequences never break a safety invariant") {
    Rng rng(20261020);
    const char* ids[] = {"LAP", "PROJ", "TV", "TV", "SIDE"};           // two identical TVs on purpose
    DisplayRouter r;
    REQUIRE(r.setOutputs({out(1), out(2, RouteKind::Duplicate, "Dup", 1), out(3, RouteKind::Independent, "Ind")}).ok());
    for (int step = 0; step < 600; ++step) {
        Topology t;
        for (int i = 0; i < 5; ++i) {
            if (rng.below(100) < 70) {
                const int rot = static_cast<int>(rng.below(4)) * 90;
                t.push_back(D(ids[i], ("c" + std::to_string(i)).c_str(), rot % 180 ? 768 : 1024, rot % 180 ? 1024 : 768, i == 0 && rng.below(2), rot));
            }
        }
        REQUIRE(r.setTopology(t).ok());
        // random operator actions against whatever is connected
        if (!t.empty() && rng.below(100) < 50) {
            const PhysicalDisplay& d = t[rng.below(static_cast<std::uint32_t>(t.size()))];
            const OutputId target = rng.below(4) == 0 ? kControlsOutputId : (rng.below(4) == 0 ? kMirrorOutputId : 1 + rng.below(3));
            if (target == kControlsOutputId) (void)r.assignControls(d.identity, d.connector);
            else if (target == kMirrorOutputId) (void)r.assignMirror(d.identity, d.connector);
            else (void)r.assignOutput(target, d.identity, d.connector);
            if (rng.below(3) == 0) (void)r.confirm(1 + rng.below(3), d.connector);
            if (rng.below(5) == 0) r.setFinalBlackout(rng.below(2) ? kAllOutputs : 1 + rng.below(3), rng.below(2) != 0);
            if (rng.below(7) == 0) (void)r.unassign(1 + rng.below(3));
        }
        const auto bad = r.invariantViolations();
        if (!bad.empty()) { INFO("step " << step << ": " << bad[0]); REQUIRE(bad.empty()); }
        for (const auto& x : r.routes()) {
            if (x.status != RouteStatus::Active) { CHECK_FALSE(x.route.connected); CHECK(x.idle); }
            if (x.status == RouteStatus::NeedsConfirmation && !x.isControls) CHECK(x.boundConnector.empty());
        }
    }
}
