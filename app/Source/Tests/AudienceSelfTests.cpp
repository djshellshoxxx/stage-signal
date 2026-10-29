// Audience-output policy through the real RenderHost (no threads, fake clock): REQ-A09 per-output BPM/key labels and
// REQ-S05/S06 published text. The compositor and overlay rules themselves are covered by the core compose tests; this file
// proves the app actually feeds them.
#include <chrono>
#include <functional>
#include <cstring>
#include <thread>
#include <memory>
#include "App/AppController.h"
#include "App/RenderHost.h"
#include "Engine/Fakes.h"
#include "Tests/SelfTest.h"
#include "UI/Live/Actions.h"
#include "UI/MediaTextPanel.h"
#include "UI/SetupPanel.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace ss::app {
namespace {
#define REQUIRE_T(c, msg) do { if (!(c)) return {false, msg}; } while (0)

struct Rig {
    ss::FakeClock clock;
    BlackoutLatch latch;
    LatestShared<ss::show::ShowSnapshot> show;
    LatestShared<SceneTable> scenes;
    ss::LatestMailbox<ss::FeatureSnapshot> features;
    ss::LatestMailbox<ss::render::AudioVisualInput> visual;
    RenderHost host;
    Rig() : host(clock, latch, show, scenes, features, visual) {
        host.setOutputs({{1, 320, 240, 0}, {2, 320, 240, 0}});
        auto snap = std::make_shared<ss::show::ShowSnapshot>();
        snap->tempo.bpm = 128.0; snap->tempo.confidence = 1.f; snap->tempo.source = ss::ValueSource::Manual;
        snap->key.tonic = 9; snap->key.minor = true; snap->key.confidence = 1.f; snap->key.source = ss::ValueSource::Manual;
        show.publish(snap);
        scenes.publish(std::make_shared<SceneTable>());
    }
    // One frame per output; returns the pixels of output `id`.
    std::vector<std::uint8_t> frame(OutputId id) {
        clock.advance(40 * ss::kNsPerMs);
        host.renderFrame();
        auto f = host.latestFrame(id);
        return f ? f->pixels.rgba : std::vector<std::uint8_t>();
    }
    void policies(bool bpmKey1, bool bpmKey2, bool text1 = true, bool text2 = true) {
        OutputPolicyEntry a; a.id = 1; a.policy.showBpmKey = bpmKey1; a.policy.showText = text1;
        OutputPolicyEntry b; b.id = 2; b.policy.showBpmKey = bpmKey2; b.policy.showText = text2;
        host.setPolicies({a, b});
    }
};

std::shared_ptr<TextRequest> textRequest(std::uint64_t seq, OutputId target, double duration = 0.0) {
    auto r = std::make_shared<TextRequest>();
    r->seq = seq; r->target = target;
    auto px = std::make_shared<ss::PixelBuffer>(64, 16);
    for (std::size_t i = 0; i < px->rgba.size(); ++i) px->rgba[i] = 255;           // opaque white block
    r->item.id = 1; r->item.utf8 = "Doors at 8"; r->item.bitmap = px; r->item.colorBitmap = true;
    r->item.x = 0.5f; r->item.y = 0.5f; r->item.size = 0.3f; r->item.fadeSec = 0.0; r->item.durationSec = duration;
    return r;
}

SelfTestResult bpmKeyPerOutput() {
    Rig r;
    r.policies(false, false);
    const auto base1 = r.frame(1), base2 = r.frame(2);
    REQUIRE_T(!base1.empty() && base1 == base2, "two identical idle outputs must render identically with the overlay off");
    r.policies(true, false);                                              // REQ-A09: enabled on output 1 only
    const auto on1 = r.frame(1), off2 = r.frame(2);
    REQUIRE_T(on1 != base1, "output 1 shows no BPM/key label although it is enabled");
    REQUIRE_T(off2 == base2, "output 2 changed although its BPM/key label is off");
    r.policies(false, false);
    REQUIRE_T(r.frame(1) == base1, "turning the label off did not restore the frame");
    // An unknown tempo and key are never drawn as numbers.
    auto none = std::make_shared<ss::show::ShowSnapshot>();
    r.show.publish(none);
    r.policies(true, true);
    REQUIRE_T(r.frame(1) == base1 && r.frame(2) == base2, "unknown tempo/key were drawn");
    return {true, "label only on the enabled output, restored when off, nothing drawn for unknown tempo/key"};
}

SelfTestResult textOnlyWhenPublishedAndTargeted() {
    Rig r;
    r.policies(false, false);
    const auto base1 = r.frame(1), base2 = r.frame(2);
    r.host.publishText(textRequest(1, 1));                                // published to output 1 only
    const auto t1 = r.frame(1), t2 = r.frame(2);
    REQUIRE_T(t1 != base1, "published text did not appear on the targeted output");
    REQUIRE_T(t2 == base2, "text appeared on an output that was not selected");
    r.host.publishText(textRequest(2, ss::kAllOutputs));                  // every output now
    REQUIRE_T(r.frame(2) != base2, "text for all outputs did not reach output 2");
    auto clear = std::make_shared<TextRequest>(); clear->seq = 3; clear->clear = true;
    r.host.publishText(clear);
    REQUIRE_T(r.frame(1) == base1 && r.frame(2) == base2, "clear did not remove the text everywhere");
    return {true, "text appears only after publish, only on the selected output, and clear removes it"};
}

SelfTestResult textPolicyAndDuration() {
    Rig r;
    r.policies(false, false, /*text1*/ false, /*text2*/ true);            // output 1 must never show text
    const auto base1 = r.frame(1), base2 = r.frame(2);
    r.host.publishText(textRequest(1, ss::kAllOutputs, /*duration*/ 1.0));
    REQUIRE_T(r.frame(1) == base1, "showText=false output drew the message");
    REQUIRE_T(r.frame(2) != base2, "showText=true output did not draw the message");
    r.clock.advance(2 * ss::kNsPerSec);                                   // past the 1 s duration
    r.frame(2);
    REQUIRE_T(r.frame(2) == base2, "timed text did not expire");
    return {true, "showText policy honoured per output, timed text expires"};
}

struct Catcher final : UiModelListener {
    std::vector<ActionOutcome> actions;
    void uiActionOutcome(const ActionOutcome& o) override { actions.push_back(o); }
    void uiCommandOutcome(const CommandOutcome&) override {}
};

SelfTestResult toggleThroughController() {
    // The Setup tab sends ToggleOverlayBpmKey. Before this module nothing handled it, so the toggle did nothing at all.
    AppConfig cfg; cfg.headless = true; cfg.startAudio = false; cfg.useSampleShow = true;
    AppController c(makeFakeAdapterSet(false), cfg);
    Catcher cat;
    c.ui().addListener(&cat);
    if (!c.start().ok()) return {false, "start"};
    auto send = [&](OutputId target, double value, ActionOutcome* out) {
        ss::Action a; a.actionId = ss::id(ss::ActionId::ToggleOverlayBpmKey); a.source = ss::ActionSource::Ui; a.value = value; a.target = target;
        const std::size_t before = cat.actions.size();
        c.ui().submitAction(a);
        for (int i = 0; i < 400; ++i) {
            c.ui().poll();
            for (std::size_t k = before; k < cat.actions.size(); ++k)
                if (cat.actions[k].actionId == a.actionId) { *out = cat.actions[k]; return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    ActionOutcome on, all, bad;
    const bool gotOn = send(kPrimaryOutput, 1.0, &on), gotAll = gotOn && send(kAllOutputs, 0.0, &all), gotBad = gotAll && send(99, 1.0, &bad);
    c.ui().removeListener(&cat);
    c.stop();
    REQUIRE_T(gotOn && gotAll && gotBad, "no outcome delivered for ToggleOverlayBpmKey");
    REQUIRE_T(on.accepted, "ToggleOverlayBpmKey on the primary output was rejected: " + on.reason);
    REQUIRE_T(all.accepted, "ToggleOverlayBpmKey for all outputs was rejected: " + all.reason);
    REQUIRE_T(!bad.accepted, "ToggleOverlayBpmKey for an output that does not exist was accepted");
    return {true, "toggle accepted for one output and for all, refused for an unknown output"};
}

// ---- operator controls: audio-loss policy, true-peak limiter opt-in, text options ------------------------------------------
struct CommandCatcher final : UiModelListener {
    std::vector<CommandOutcome> commands;
    void uiActionOutcome(const ActionOutcome&) override {}
    void uiCommandOutcome(const CommandOutcome& o) override { commands.push_back(o); }
};

bool waitPoll(AppController& c, const std::function<bool()>& done, int ms) {
    for (int t = 0; t < ms; t += 10) { c.ui().poll(); if (done()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    c.ui().poll();
    return done();
}

SelfTestResult prefsAreValidated() {
    AppConfig cfg; cfg.headless = true; cfg.startAudio = false;
    AppController c(makeFakeAdapterSet(false), cfg);
    CommandCatcher cat;
    c.ui().addListener(&cat);
    if (!c.start().ok()) return {false, "start"};
    auto send = [&](const char* key, const char* value, bool* accepted) {
        AppCommand cmd; cmd.kind = AppCommandKind::SetPref; cmd.text = key; cmd.text2 = value;
        std::uint64_t id = 0;
        if (!c.ui().postCommand(cmd, &id)) return false;
        return waitPoll(c, [&] { for (const auto& o : cat.commands) if (o.correlationId == id) { *accepted = o.ok; return true; } return false; }, 3000);
    };
    struct Row { const char* key; const char* value; bool expect; };
    const Row rows[] = {
        {"audio.losspolicy", "idle", true}, {"audio.losspolicy", "continue", true}, {"audio.losspolicy", "bogus", false}, {"audio.losspolicy", "", false},
        {"dynamics.truepeak", "1", true}, {"dynamics.truepeak", "yes", false},
        {"text.duration", "0", true}, {"text.duration", "10", true}, {"text.duration", "0.05", false}, {"text.duration", "5000", false}, {"text.duration", "abc", false},
        {"text.duration", "nan", false}, {"text.fade", "0.5", true}, {"text.fade", "-1", false}, {"text.scroll", "1", true}, {"text.scroll", "2", false},
        {"text.y", "0.9", true}, {"text.y", "1.5", false}, {"text.size", "0.2", true}, {"text.size", "0", false},
        {"streamkey.secret", "x", false}, {"broadcast.host", "evil", false}};
    std::string bad;
    for (const auto& r : rows) {
        bool accepted = !r.expect;
        if (!send(r.key, r.value, &accepted)) { bad = std::string("no outcome for ") + r.key; break; }
        if (accepted != r.expect) { bad = std::string(r.key) + "=" + r.value + (r.expect ? " was refused" : " was accepted"); break; }
    }
    const std::string lossPref = c.pref("audio.losspolicy");      // last accepted value is "continue"
    const std::string rejectedStaysOut = c.pref("text.size");     // "0" was refused, so the earlier 0.2 stays
    c.ui().removeListener(&cat);
    c.stop();
    if (!bad.empty()) return {false, bad};
    if (lossPref != "continue") return {false, "refused value replaced a saved preference (audio.losspolicy=" + lossPref + ")"};
    if (rejectedStaysOut != "0.2") return {false, "refused value replaced a saved preference (text.size=" + rejectedStaysOut + ")"};
    return {true, "22 values checked: allowed ones accepted, out-of-range, malformed and unknown keys refused, refusals change nothing"};
}

SelfTestResult operatorControlsWritePrefs() {
    AppConfig cfg; cfg.headless = true; cfg.startAudio = false;
    AppController c(makeFakeAdapterSet(false), cfg);
    if (!c.start().ok()) return {false, "start"};
    if (!waitPoll(c, [&] { return c.ui().snapshot() != nullptr; }, 3000)) return {false, "no snapshot"};
    SetupPanel setup(c.ui());
    setup.setBounds(0, 0, 800, 456);
    MediaTextPanel media(c.ui());
    media.setBounds(0, 0, 800, 456);
    c.ui().poll();
    auto* loss = dynamic_cast<juce::ComboBox*>(uikit::findById(&setup, "setup.audio.losspolicy"));
    auto* tp = dynamic_cast<juce::Button*>(uikit::findById(&setup, "setup.audio.truepeak"));
    auto* dur = dynamic_cast<juce::ComboBox*>(uikit::findById(&media, "text.duration"));
    auto* fade = dynamic_cast<juce::ComboBox*>(uikit::findById(&media, "text.fade"));
    auto* scroll = dynamic_cast<juce::Button*>(uikit::findById(&media, "text.scroll"));
    if (!loss || !tp || !dur || !fade || !scroll) return {false, "an operator control is missing"};
    if (!loss->isEnabled() || !tp->isEnabled() || !dur->isEnabled()) return {false, "a control is disabled"};
    loss->setSelectedId(2, juce::sendNotificationSync);
    tp->setToggleState(true, juce::dontSendNotification); if (tp->onClick) tp->onClick();
    dur->setSelectedId(3, juce::sendNotificationSync);       // 10 seconds
    fade->setSelectedId(4, juce::sendNotificationSync);      // 2 s
    scroll->setToggleState(true, juce::dontSendNotification); if (scroll->onClick) scroll->onClick();
    const bool saved = waitPoll(c, [&] {
        return c.pref("audio.losspolicy") == "idle" && c.pref("dynamics.truepeak") == "1" && c.pref("text.duration") == "10" &&
               c.pref("text.fade") == "2" && c.pref("text.scroll") == "1";
    }, 3000);
    c.stop();
    return saved ? SelfTestResult{true, "audio-loss policy, true-peak opt-in and the three text options reach the saved preferences"}
                 : SelfTestResult{false, "a control did not reach the saved preferences"};
}
}  // namespace

void registerAudienceSelfTests(SelfTestRegistry& r) {
    r.add("audience: REQ-A09 BPM and key label only where enabled", bpmKeyPerOutput);
    r.add("audience: REQ-S05 text only when published and only on the selected output", textOnlyWhenPublishedAndTargeted);
    r.add("audience: REQ-S06 showText policy and timed expiry", textPolicyAndDuration);
    r.add("audience: ToggleOverlayBpmKey reaches the router", toggleThroughController);
    r.add("audience: SetPref validates policy, limiter and text options", prefsAreValidated);
    r.add("audience: operator controls write the saved preferences", operatorControlsWritePrefs);
}

}  // namespace ss::app
