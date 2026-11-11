// MOD-05 OutputRouter: identity routing, never covering controls, ambiguous monitors, blackout finalize.
#include "app_fakes.h"
using namespace apptest;

static std::vector<show::Output> outs() {
  show::Output a; a.logicalId = "main"; show::Output b; b.logicalId = "alt"; show::Output c; c.logicalId = "ctl"; c.role = show::OutputRole::Controls;
  show::Output p; p.logicalId = "prog"; p.role = show::OutputRole::Program;
  return {a, b, c, p};
}
static DisplayInfo D(const std::string& id, int idx, int x, bool prim = false, bool amb = false) { return FakeDisplays::disp(id, idx, x, 800, 600, prim, amb); }
static const OutputRouteView* V(const std::vector<OutputRouteView>& v, const std::string& id) { for (auto& x : v) if (x.logicalId == id) return &x; return nullptr; }

TEST(router_infers_controls_primary_and_never_auto_assigns) {
  OutputRouter r; r.setOutputs(outs());
  r.updateDisplays({D("A", 0, 0, true), D("B", 1, 800)});
  CHECK(r.controlsDisplay() == "A");
  auto v = r.views();
  CHECK(V(v, "main")->state == RouteState::Unassigned);      // nothing is auto-assigned to a display
  CHECK(V(v, "ctl")->state == RouteState::ControlsHost);
  CHECK(V(v, "prog")->state == RouteState::Virtual);          // program composition exists without a window
  CHECK(r.assignById("main", "B").ok());
  CHECK(V(r.views(), "main")->state == RouteState::Active);
}

TEST(router_never_covers_controls_display) {
  OutputRouter r; r.setOutputs(outs()); r.updateDisplays({D("A", 0, 0, true), D("B", 1, 800)});
  Status s = r.assignById("main", "A");
  CHECK(!s.ok()); CHECK(s.code == Err::PermissionDenied);
  // persisted assignment onto the controls display (e.g. controls were moved) must be Blocked, never rendered there
  r.setAssignments({{"main", "A"}});
  CHECK(V(r.views(), "main")->state == RouteState::Blocked);
  auto t = r.table(); bool renders = false; for (auto& d : t->routes) if (d.logicalId == "main" && d.rendersFrames()) renders = true;
  CHECK(!renders);
}

TEST(router_disconnect_reconnect_by_identity_not_index) {
  OutputRouter r; r.setOutputs(outs()); r.updateDisplays({D("A", 0, 0, true), D("B", 1, 800), D("C", 2, 1600)});
  CHECK(r.assignById("main", "B").ok());
  uint64_t g0 = V(r.views(), "main")->generation;
  r.updateDisplays({D("A", 0, 0, true), D("C", 1, 800)});                      // B unplugged; C now has index 1
  auto v = r.views();
  CHECK(V(v, "main")->state == RouteState::Disconnected);
  CHECK(V(v, "ctl")->state == RouteState::ControlsHost);                       // controls untouched
  CHECK(r.controlsDisplay() == "A");
  CHECK(V(v, "alt")->state == RouteState::Unassigned);
  r.updateDisplays({D("A", 0, 0, true), D("C", 1, 800), D("B", 2, 1600)});       // B returns at a different index/position
  v = r.views();
  CHECK(V(v, "main")->state == RouteState::Active);
  CHECK(V(v, "main")->displayId == "B");
  CHECK(V(v, "main")->generation != g0);                                        // windows must be re-created (new generation)
  CHECK(V(v, "alt")->state == RouteState::Unassigned);                          // C was never silently adopted
}

TEST(router_controls_display_lost_does_not_move_audience_onto_anything) {
  OutputRouter r; r.setOutputs(outs()); r.updateDisplays({D("A", 0, 0, true), D("B", 1, 800)});
  CHECK(r.assignById("main", "B").ok());
  r.updateDisplays({D("B", 0, 0, true)});                                       // controls display A gone; B becomes primary
  CHECK(r.controlsDisplay() == "A");                                            // reserved identity is not re-inferred
  auto v = r.views();
  CHECK(V(v, "main")->state == RouteState::Active);                             // audience stays where it is
  CHECK(V(v, "ctl")->state == RouteState::Disconnected);
}

TEST(router_identical_monitors_idle_until_reassigned) {
  OutputRouter r; r.setOutputs(outs());
  r.updateDisplays({D("A", 0, 0, true), D("M", 1, 800, false, true), D("M", 2, 1600, false, true)});
  CHECK(!r.assignById("main", "M").ok());                                       // ambiguous identity cannot be assigned by id alone
  r.setAssignments({{"main", "M"}});                                            // e.g. loaded from LocalSettings after restart
  CHECK(V(r.views(), "main")->state == RouteState::NeedsReassign);
  auto t = r.table(); for (auto& d : t->routes) if (d.logicalId == "main") CHECK(!d.rendersFrames());
  DisplayInfo second = D("M", 2, 1600, false, true);
  CHECK(r.assign("main", second).ok());                                         // operator picks the physical screen
  auto v = r.views(); CHECK(V(v, "main")->state == RouteState::Active); CHECK(V(v, "main")->x == 1600);
  r.updateDisplays({D("A", 0, 0, true), D("M", 1, 800, false, true)});          // one of the identical monitors unplugged
  CHECK(V(r.views(), "main")->state == RouteState::NeedsReassign);
  r.updateDisplays({D("A", 0, 0, true), D("M", 1, 800, false, true), D("M", 2, 1600, false, true)});
  CHECK(V(r.views(), "main")->state == RouteState::NeedsReassign);              // topology changed in between: stays idle until confirmed
  CHECK(r.assign("main", second).ok());
  CHECK(V(r.views(), "main")->state == RouteState::Active);
}

TEST(router_two_outputs_cannot_share_a_display_and_resolution_change_bumps_generation) {
  OutputRouter r; r.setOutputs(outs()); r.updateDisplays({D("A", 0, 0, true), D("B", 1, 800)});
  CHECK(r.assignById("main", "B").ok());
  CHECK(!r.assignById("alt", "B").ok());
  uint64_t g = V(r.views(), "main")->generation;
  DisplayInfo b = D("B", 1, 800); b.w = 1280; b.h = 720; b.rotation = 90;
  r.updateDisplays({D("A", 0, 0, true), b});
  auto v = r.views(); CHECK(V(v, "main")->state == RouteState::Active); CHECK(V(v, "main")->w == 1280); CHECK(V(v, "main")->generation != g); CHECK(V(v, "main")->rotation == 90);
}

TEST(router_blackout_final_composition_skips_controls_and_diagnostics_default_off) {
  OutputRouter r; r.setOutputs(outs()); r.updateDisplays({D("A", 0, 0, true), D("B", 1, 800)}); r.assignById("main", "B");
  CHECK(!r.diagnostics("main"));
  Frame f; f.resize(8, 8); for (auto& p : f.px) p = 0xFFFFFF;
  RouteDesc aud; aud.role = show::OutputRole::Audience; RouteDesc ctl; ctl.role = show::OutputRole::Controls;
  CHECK(!r.finalize(aud, f));
  r.setBlackout(true);
  CHECK(!r.finalize(ctl, f)); CHECK(f.px[0] == 0xFFFFFF);
  CHECK(r.finalize(aud, f)); bool z = true; for (auto p : f.px) if (p) z = false; CHECK(z);
  CHECK(V(r.views(), "main")->blackedOut); CHECK(!V(r.views(), "ctl")->blackedOut);
}
int main() { return ss_test::runAll(); }
