#include "app_fakes.h"
using namespace apptest;

TEST(smoke_start_pump_status) {
  Rig r;
  r.run(200);
  AppStatus s = r.ctl->status();
  CHECK(s.showName == "Untitled show");
  CHECK(!s.dmxArmed);
  CHECK(s.recState == RecState::Idle);
  CHECK(s.liveState == BroadcastState::Off);
  CHECK(s.framesRendered > 0);
  FramePtr f; CHECK(r.ctl->previewFrame(f));
  auto rep = r.ctl->shutdown();
  for (auto& st : rep.stages) std::printf("  stage %s ok=%d %lldms %s\n", st.name.c_str(), st.ok, (long long)st.elapsedMs, st.detail.c_str());
  CHECK(rep.clean);
}
int main() { return ss_test::runAll(); }
