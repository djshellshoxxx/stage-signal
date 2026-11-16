// Owner: ui-live. Self-tests for the Live tab, status bar, performance strip, shortcuts and audience frames.
// Panels are built against a headless AppController with fake adapters; no hardware, no sleeps beyond polling.
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/AppController.h"
#include "Engine/AdapterSet.h"
#include "Engine/Fakes.h"
#include "Tests/SelfTest.h"
#include "UI/Audience/FrameImage.h"
#include "UI/Live/Actions.h"
#include "UI/Live/LiveFormat.h"
#include "UI/Live/ScenePad.h"
#include "UI/Live/Shortcuts.h"
#include "UI/LivePanel.h"
#include "UI/PerformanceStrip.h"
#include "UI/Status/StatusFormat.h"
#include "UI/StatusBar.h"

namespace ss::app {
namespace {

struct OutcomeCatcher final : UiModelListener {
    std::vector<ActionOutcome> got;
    void uiActionOutcome(const ActionOutcome& o) override { got.push_back(o); }
};

// Clicks a button synchronously: flips a toggle as a real click would, then runs its onClick.
// (Button::triggerClick is asynchronous and this build has no modal loop to pump it.)
void click(juce::Button& b) {
    if (b.getClickingTogglesState()) b.setToggleState(!b.getToggleState(), juce::dontSendNotification);
    if (b.onClick) b.onClick();
}

bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
    for (int t = 0; t < timeoutMs; t += 10) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

bool hasOutcome(const OutcomeCatcher& c, std::uint32_t actionId) {
    for (const auto& o : c.got)
        if (o.actionId == actionId) return true;
    return false;
}

// Operator copy: no em dash (U+2014) or en dash (U+2013), no banned phrase (app-ui 3.5).
std::string copyProblem(const std::string& s) {
    if (s.find("\xE2\x80\x94") != std::string::npos) return "em dash in '" + s + "'";
    if (s.find("\xE2\x80\x93") != std::string::npos) return "en dash in '" + s + "'";
    std::string low;
    for (char ch : s) low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    for (const char* bad : {"seamless", "effortless", "unlock", "elevate", "empower", "leverage", "robust", "delve", "journey", "oops"})
        if (low.find(bad) != std::string::npos) return "banned word '" + std::string(bad) + "'";
    return {};
}

SelfTestResult fail(const std::string& why) { return SelfTestResult{false, why}; }

SelfTestResult labels() {
    ss::TempoState t;
    t.bpm = 128.0;
    t.source = ss::ValueSource::Estimated;
    t.confidence = 0.8f;
    if (live::tempoText(t) != "128.0 BPM") return fail("tempoText: " + live::tempoText(t));
    if (live::tempoText(ss::TempoState{}) != "BPM unknown") return fail("tempo unknown label");
    ss::KeyState k;
    k.tonic = 9;
    k.minor = true;
    k.source = ss::ValueSource::Estimated;
    if (live::keyText(k) != "A minor") return fail("keyText: " + live::keyText(k));
    if (live::cueText(2, 12) != "Cue 3 of 12") return fail("cueText: " + live::cueText(2, 12));
    if (live::bankText(0, 3) != "Bank 1 of 3") return fail("bankText");
    if (live::percentText(0.5) != "50%") return fail("percentText");
    if (live::confidencePercent(0.8f) != 80) return fail("confidencePercent");

    UiSnapshot s;
    s.audio.deviceState = AdapterState::Unavailable;
    if (status::audioChip(s).name() != "Audio: No device") return fail("audio chip: " + status::audioChip(s).name());
    s.audio.deviceState = AdapterState::Running;
    s.audio.signalAlive = true;
    s.features.silent = false;
    s.features.rmsDb[0] = -14.f;
    if (status::audioChip(s).value != "-14 dB") return fail("audio level: " + status::audioChip(s).value);
    s.lighting.state = LightingUiState::Armed;
    if (status::lightingChip(s).name() != "Lighting: Armed") return fail("lighting chip");
    s.record.state = RecordUiState::Recording;
    s.record.elapsedMs = 754000;   // 12 minutes 34 seconds
    if (status::recordChip(s).value != "12:34") return fail("record clock: " + status::recordChip(s).value);
    s.show.visualBlackout = true;

    for (const auto& text : {live::tempoText(t), live::keyText(k), live::cueText(2, 12), status::audioChip(s).name(),
                             status::lightingChip(s).name(), status::recordChip(s).name()}) {
        const auto p = copyProblem(text);
        if (!p.empty()) return fail(p);
    }
    return {};
}

SelfTestResult audienceFrameCopy() {
    ss::PixelBuffer px(4, 2);
    for (std::uint32_t y = 0; y < 2; ++y)
        for (std::uint32_t x = 0; x < 4; ++x) {
            std::uint8_t* p = px.row(y) + x * 4;
            p[0] = static_cast<std::uint8_t>(10 + x);
            p[1] = 20;
            p[2] = 30;
            p[3] = 255;
        }
    juce::Image img;
    audience::copyFrame(px, img);
    if (img.getWidth() != 4 || img.getHeight() != 2) return fail("copyFrame size");
    const auto c = img.getPixelAt(2, 1);
    if (c.getRed() != 12 || c.getGreen() != 20 || c.getBlue() != 30) return fail("copyFrame colour");
    const auto* before = img.getPixelData().get();
    audience::copyFrame(px, img);   // same size: no new image
    if (img.getPixelData().get() != before) return fail("copyFrame reallocated on a same-size frame");
    return {};
}

SelfTestResult panelsWithController() {
    AdapterSet adapters = makeFakeAdapterSet(/*withSine=*/true);
    AppConfig cfg;
    cfg.headless = true;
    AppController c(adapters, cfg);
    OutcomeCatcher catcher;
    c.ui().addListener(&catcher);
    if (!c.start().ok()) return fail("start() failed");
    if (!waitFor([&] { return c.ui().version() > 0; }, 2000)) return fail("no UI snapshot");

    // Panels are destroyed before the controller (declared after it).
    StatusBar bar(c.ui());
    LivePanel live(c.ui());
    PerformanceStrip strip(c.ui());
    bar.setBounds(0, 0, 800, 48);
    live.setBounds(0, 48, 800, 456);
    strip.setBounds(0, 540, 800, 60);
    c.ui().poll();

    if (bar.chipCount() != 10) return fail("status bar chip count");
    if (bar.chip(0).label != "Show") return fail("first chip is not Show");
    if (strip.visualText() != "Visual") return fail("strip visual label before any latch: " + strip.visualText().toStdString());
    if (live.target() != ss::kAllOutputs) return fail("Live target does not start at all outputs");

    auto* tap = dynamic_cast<juce::Button*>(uikit::findById(&live, "live.tap"));
    if (!tap) return fail("Tap button missing");
    click(*tap);
    if (!waitFor([&] { c.ui().poll(); return hasOutcome(catcher, ss::id(ss::ActionId::TapTempo)); }, 1000))
        return fail("Tap did not reach the controller");

    auto* pad = dynamic_cast<live::ScenePad*>(uikit::findById(&live, "live.pad.3"));
    if (!pad) return fail("scene pad 3 missing");
    pad->onPress(2);   // slot 0..7
    if (!waitFor([&] { c.ui().poll(); return hasOutcome(catcher, ss::id(ss::ActionId::SceneSelect)); }, 1000))
        return fail("scene pad did not send SceneSelect");

    if (!live::handleShortcut(c.ui(), juce::KeyPress('5'))) return fail("key 5 not bound");
    if (!live::handleShortcut(c.ui(), juce::KeyPress('t'))) return fail("key T not bound");
    if (live::handleShortcut(c.ui(), juce::KeyPress('q'))) return fail("unbound key was handled");

    // Blackout latch through the strip button, then restore from Shift+F8 (app-ui 4.2, 6).
    auto* visual = dynamic_cast<juce::Button*>(uikit::findById(&strip, "strip.visual"));
    if (!visual) return fail("visual blackout button missing");
    click(*visual);
    if (!waitFor([&] { c.ui().poll(); return strip.visualText() == "Visual ON"; }, 1500))
        return fail("strip did not show the latched state: " + strip.visualText().toStdString());
    if (!strip.restoreEnabled()) return fail("Restore not enabled while latched");
    if (!live::handleShortcut(c.ui(), juce::KeyPress(juce::KeyPress::F8Key, juce::ModifierKeys(juce::ModifierKeys::shiftModifier), 0)))
        return fail("Shift+F8 not bound");
    if (!waitFor([&] { c.ui().poll(); return strip.visualText() == "Visual"; }, 1500))
        return fail("strip did not clear after Restore");

    const auto problem = copyProblem(strip.visualText().toStdString() + live.noticeText().toStdString());
    c.ui().removeListener(&catcher);
    c.stop();
    if (!problem.empty()) return fail(problem);
    return {};
}

}  // namespace

void registerUiLiveSelfTests(SelfTestRegistry& r) {
    r.add("ui-live: labels", labels);
    r.add("ui-live: audience frame copy", audienceFrameCopy);
    r.add("ui-live: panels with controller", panelsWithController);
}

}  // namespace ss::app
