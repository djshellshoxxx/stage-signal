#include "rig.hpp"
#include "ss/show/audio_health.hpp"
using namespace sst;

namespace {
Tunables idleTun() {
    Tunables t = fastTun();
    t.audioLossPolicy = AudioLossPolicy::Idle;
    return t;
}
}  // namespace

TEST_CASE("REQ-D09: monitor confirms loss only after the detect time and never before") {
    AudioHealthConfig c;
    c.detectNs = msToNs(1000); c.restoreHoldNs = msToNs(500);
    AudioHealthMonitor m(c);
    CHECK_FALSE(m.update(0, true).lost);
    CHECK_FALSE(m.update(msToNs(100), false).lost);           // loss starts here
    CHECK_FALSE(m.update(msToNs(1099), false).lost);          // 999 ms: not yet
    const auto s = m.update(msToNs(1100), false);             // 1000 ms: confirmed
    CHECK(s.lost);
    CHECK(s.changed);
}

TEST_CASE("REQ-D09: a short dropout shorter than the detect time is ignored") {
    AudioHealthMonitor m(AudioHealthConfig{});
    m.update(0, true);
    m.update(msToNs(100), false);
    m.update(msToNs(600), true);                              // came back after 500 ms
    CHECK_FALSE(m.update(msToNs(3000), true).lost);
    CHECK_FALSE(m.update(msToNs(3100), false).lost);          // a second dropout restarts the timer
}

TEST_CASE("REQ-D09: restore needs a stable hold so a flapping device does not flicker the output") {
    AudioHealthConfig c;
    c.detectNs = msToNs(200); c.restoreHoldNs = msToNs(500);
    AudioHealthMonitor m(c);
    m.update(0, false);
    REQUIRE(m.update(msToNs(200), false).lost);
    CHECK(m.update(msToNs(300), true).lost);                  // back, but not held yet
    CHECK(m.update(msToNs(500), false).lost);                 // flaps away again: still lost, hold restarts
    CHECK(m.update(msToNs(600), true).lost);
    CHECK(m.update(msToNs(1000), true).lost);                 // 400 ms of good audio
    const auto s = m.update(msToNs(1100), true);              // 500 ms
    CHECK_FALSE(s.lost);
    CHECK(s.changed);
}

TEST_CASE("REQ-D09: config is validated and the 2 s guarantee cannot be configured away") {
    AudioHealthConfig c;
    CHECK(validate(c).ok());
    c.detectNs = msToNs(2001);
    CHECK_FALSE(validate(c).ok());
    c.detectNs = 0;
    CHECK_FALSE(validate(c).ok());
    c = AudioHealthConfig{};
    c.restoreHoldNs = -1;
    CHECK_FALSE(validate(c).ok());
    c.restoreHoldNs = secToNs(11);
    CHECK_FALSE(validate(c).ok());
    // an invalid config handed to the monitor falls back to defaults rather than disabling detection
    c = AudioHealthConfig{}; c.detectNs = secToNs(60);
    AudioHealthMonitor m(c);
    m.update(0, false);
    CHECK(m.update(msToNs(2000), false).lost);
}

TEST_CASE("REQ-D09: time going backwards or a huge jump cannot confirm loss early or wrap") {
    AudioHealthMonitor m(AudioHealthConfig{});
    m.update(secToNs(10), false);
    CHECK_FALSE(m.update(secToNs(5), false).lost);            // clock moved back: treated as no elapsed time
    CHECK(m.update(secToNs(5) + secToNs(3), false).lost);
}

TEST_CASE("REQ-D09: continue policy keeps the show running and only reports the loss") {
    Rig r(mkDoc(3));
    r.eng.setAudioAvailable(false);
    r.run(secToNs(3));
    CHECK(r.snap.audioLost);
    CHECK_FALSE(r.snap.audioIdle);
    CHECK(r.count(EventKind::AudioLost) == 1);
    CHECK(r.count(EventKind::SceneChanged) >= 1);             // auto show keeps changing scenes
}

TEST_CASE("REQ-D09: idle policy shows the idle screen within 2 s of loss and restores after the hold") {
    Rig r(mkDoc(3), idleTun());
    CHECK_FALSE(r.snap.audioIdle);
    r.eng.setAudioAvailable(false);
    r.step(0);
    r.run(msToNs(1900));
    CHECK(r.snap.audioIdle);                                  // default detect time is 1 s: well inside 2 s
    CHECK(r.snap.audioLost);
    r.eng.setAudioAvailable(true);
    r.run(msToNs(300));
    CHECK(r.snap.audioIdle);                                  // hold not elapsed
    r.run(secToNs(1));
    CHECK_FALSE(r.snap.audioIdle);
    CHECK(r.count(EventKind::AudioLost) == 1);
    CHECK(r.count(EventKind::AudioRestored) == 1);
}

TEST_CASE("REQ-D09: idle never changes the document, blackout latches or the current scene") {
    Rig r(mkDoc(3), idleTun());
    r.act(ActionId::SceneSelect, 2); r.step(0);
    const auto scene = r.scene();
    const auto rev = r.snap.revision;
    r.eng.setAudioAvailable(false);
    r.run(secToNs(2));
    REQUIRE(r.snap.audioIdle);
    CHECK_FALSE(r.snap.visualBlackout);
    CHECK_FALSE(r.snap.emergencyBlackout);
    CHECK(r.snap.revision == rev);                            // loss is a transient, not a document edit
    CHECK(r.scene() == scene);
}

TEST_CASE("REQ-D09: loss events carry no device names or paths and are journaled as device events") {
    Rig r(mkDoc(2), idleTun());
    std::vector<model::JournalEntry> j;
    r.eng.setAudioAvailable(false);
    r.run(secToNs(2));
    r.eng.drainJournal(j);
    bool found = false;
    for (const auto& e : j) if (e.kind == "device" && e.detail.find("AudioLost") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("REQ-D09: audio is assumed present until the app says otherwise") {
    Rig r(mkDoc(2), idleTun());
    r.run(secToNs(10));
    CHECK_FALSE(r.snap.audioLost);
    CHECK_FALSE(r.snap.audioIdle);
}
