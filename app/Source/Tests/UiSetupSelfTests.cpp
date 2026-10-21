// Owner: ui-setup. Self-tests for the Setup tab and the first-run wizard. Fake adapters only; nothing transmits.
#include <string>
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/AppController.h"
#include "Engine/AdapterSet.h"
#include "Engine/Fakes.h"
#include "Tests/SelfTest.h"
#include "UI/Audience/OutputManager.h"
#include "UI/Live/Actions.h"
#include "UI/Setup/AppearanceSettings.h"
#include "UI/Setup/Wizard.h"
#include "UI/SetupPanel.h"

namespace ss::app {
namespace {

SelfTestResult fail(const std::string& why) { return SelfTestResult{false, why}; }

// Clicks a button synchronously: flips a toggle as a real click would, then runs its onClick.
// (Button::triggerClick is asynchronous and this build has no modal loop to pump it.)
void click(juce::Button& b) {
    if (b.getClickingTogglesState()) b.setToggleState(!b.getToggleState(), juce::dontSendNotification);
    if (b.onClick) b.onClick();
}

SelfTestResult wizardKeyboardPath() {
    UiModel model;
    int finished = 0;
    setup::Wizard w(model, [&] { ++finished; }, [] {});
    if (w.step() != 0) return fail("wizard does not start at step 1");
    w.next();                                  // welcome -> displays
    w.next();                                  // displays -> audio
    if (w.canAdvance()) return fail("audio step advanced without a signal or a skip");
    UiSnapshot quiet;
    quiet.features.peakDb[0] = -60.f;
    w.observe(quiet);
    if (w.canAdvance()) return fail("quiet input counted as a signal");
    UiSnapshot loud;
    loud.features.peakDb[0] = -20.f;
    w.observe(loud);
    if (!w.canAdvance()) return fail("signal above minus 40 dB did not complete the audio step");
    w.next();                                  // audio -> camera
    w.skip();                                  // camera skipped
    w.skip();                                  // MIDI skipped
    w.next();                                  // lighting -> recording folder
    w.skip();                                  // recording folder skipped
    if (w.step() != 7) return fail("expected step 8 of 8, got " + std::to_string(w.step() + 1));
    w.next();                                  // summary: Finish
    if (finished != 1) return fail("Finish did not run");

    const auto sum = w.summary();
    bool lightingOff = false;
    for (const auto& s : sum.off) lightingOff = lightingOff || s.find("Lighting: Disarmed") != std::string::npos;
    if (!lightingOff) return fail("summary does not say lighting stays Disarmed");
    if (sum.skipped.empty()) return fail("skipped steps are not listed");
    return {};
}

SelfTestResult wizardEscapeAsksFirst() {
    UiModel model;
    setup::Wizard w(model, [] {}, [] {});
    w.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey, juce::ModifierKeys(), 0));
    if (!w.findChildWithID("wiz.keep")) return fail("no Keep button on the leave prompt");
    if (!w.findChildWithID("wiz.keep")->isVisible()) return fail("leave prompt not shown on Esc");
    if (w.step() != 0) return fail("Esc moved the wizard");
    return {};
}

SelfTestResult setupPanelWithController() {
    AdapterSet adapters = makeFakeAdapterSet(/*withSine=*/true);
    AppConfig cfg;
    cfg.headless = true;
    AppController c(adapters, cfg);
    if (!c.start().ok()) return fail("start() failed");
    for (int i = 0; i < 200 && c.ui().version() == 0; ++i) juce::Thread::sleep(10);

    SetupPanel panel(c.ui());
    panel.setBounds(0, 0, 800, 456);
    c.ui().poll();

    auto* strobe = dynamic_cast<juce::Button*>(uikit::findById(&panel, "setup.safety.strobe"));
    if (!strobe) return fail("strobe toggle missing");
    if (strobe->getToggleState() != c.ui().snapshot()->show.strobingDisabled) return fail("toggle does not match the snapshot");
    const bool before = strobe->getToggleState();
    click(*strobe);   // sends DisableStrobing or EnableStrobing through the action registry
    if (strobe->getToggleState() == before) return fail("strobe toggle did not change on click");

    auto* wizardButton = dynamic_cast<juce::Button*>(uikit::findById(&panel, "setup.wizard"));
    if (!wizardButton) return fail("wizard button missing");
    click(*wizardButton);
    if (!panel.wizardActive()) return fail("wizard did not open");
    if (panel.wizard()->step() != 0) return fail("wizard does not start at step 1");

    // Theme switch goes through the appearance settings (session only), and back.
    auto* theme = dynamic_cast<juce::ComboBox*>(uikit::findById(&panel, "setup.theme"));
    if (!theme) return fail("theme choice missing");
    theme->setSelectedId(2, juce::sendNotificationSync);
    if (setup::appearance().theme != setup::ThemeChoice::HighContrast) return fail("high contrast not stored");
    theme->setSelectedId(1, juce::sendNotificationSync);
    if (setup::appearance().theme != setup::ThemeChoice::Night) return fail("night theme not restored");
    c.stop();
    return {};
}

SelfTestResult displayGuard() {
    AdapterSet adapters = makeFakeAdapterSet(/*withSine=*/false);
    AppConfig cfg;
    cfg.headless = true;
    AppController c(adapters, cfg);
    if (!c.start().ok()) return fail("start() failed");
    audience::OutputManager outputs(c.ui());
    const auto& displays = juce::Desktop::getInstance().getDisplays().displays;
    if (displays.isEmpty()) return fail("no display in the test environment");
    // A component on the first display stands in for the operator window: an audience window must refuse that display.
    juce::Component operatorStandIn;
    operatorStandIn.setBounds(10, 10, 100, 100);
    juce::String reason;
    if (outputs.open(2, displays.getReference(0), &operatorStandIn, reason)) return fail("audience window opened on the operator display");
    if (reason.isEmpty()) return fail("refusal has no reason");
    if (outputs.openCount() != 0) return fail("a window was created despite the refusal");
    c.stop();
    return {};
}

}  // namespace

void registerUiSetupSelfTests(SelfTestRegistry& r) {
    r.add("ui-setup: wizard keyboard path", wizardKeyboardPath);
    r.add("ui-setup: wizard escape asks first", wizardEscapeAsksFirst);
    r.add("ui-setup: setup panel with controller", setupPanelWithController);
    r.add("ui-setup: audience display guard", displayGuard);
}

}  // namespace ss::app
