// Owner: ui-setup. Self-tests for the Setup tab and the first-run wizard. Fake adapters only; nothing transmits.
#include <string>
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/AppController.h"
#include "Engine/AdapterSet.h"
#include "Engine/Fakes.h"
#include "Tests/SelfTest.h"
#include "UI/Audience/OutputManager.h"
#include "UI/Audience/DisplaySafety.h"
#include "UI/Live/Actions.h"
#include "UI/Setup/AppearanceSettings.h"
#include "UI/Setup/Wizard.h"
#include "UI/Setup/WizardProgress.h"
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
    AppCommand saved;
    setup::WizardProgress progress;
    bool completionSaved = false;
    while (model.popCommand(saved)) if (saved.kind == AppCommandKind::SetPref && saved.text == "wizard.progress") {
        if (!setup::WizardProgress::decode(saved.text2, progress)) return fail("saved wizard progress malformed");
        completionSaved = progress.completed;
    }
    if (!completionSaved || !progress.audioSeen) return fail("wizard completion/audio verification was not persisted");

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
    if (outputs.open(2, displays.getReference(0), nullptr, reason)) return fail("unknown operator position was accepted");
    auto disconnected = displays.getReference(0);
    disconnected.logicalBounds = disconnected.logicalBounds.translated(10000.f, 0.f);
    if (outputs.open(2, disconnected, &operatorStandIn, reason)) return fail("stale disconnected display was accepted");
    c.stop();
    return {};
}

}  // namespace

void registerUiSetupSelfTests(SelfTestRegistry& r) {
    r.add("ui-setup: wizard keyboard path", wizardKeyboardPath);
    r.add("ui-setup: wizard escape asks first", wizardEscapeAsksFirst);
    r.add("ui-setup: wizard resumes saved progress", [] {
        UiModel model;
        auto snapshot = std::make_shared<UiSnapshot>();
        setup::WizardProgress saved; saved.step = 4; saved.audioSkipped = true; saved.results[2] = 2;
        snapshot->prefs.emplace_back("wizard.progress", saved.encode()); model.publish(snapshot);
        setup::Wizard wizard(model, [] {}, [] {});
        if (wizard.step() != 4) return fail("wizard did not resume at persisted step");
        setup::WizardProgress decoded;
        if (setup::WizardProgress::decode("999999999999999", decoded)) return fail("corrupt progress accepted");
        return SelfTestResult{true, "saved step resumes; malformed progress rejected"};
    });
    r.add("ui-setup: setup panel with controller", setupPanelWithController);
    r.add("ui-setup: audience display guard", displayGuard);
    r.add("ui-setup: audience topology safety", [] {
        juce::Displays::Display controls{}, audienceDisplay{};
        controls.isMain = true; controls.scale = 1.;
        controls.logicalBounds = {0.f, 0.f, 800.f, 600.f};
        controls.physicalBounds = {0, 0, 800, 600};
        audienceDisplay.isMain = false; audienceDisplay.scale = 1.;
        audienceDisplay.logicalBounds = {800.f, 0.f, 800.f, 600.f};
        audienceDisplay.physicalBounds = {800, 0, 800, 600};
        juce::Array<juce::Displays::Display> before;
        before.add(controls); before.add(audienceDisplay);
        juce::String reason;
        if (!audience::audiencePlacementAllowed(audienceDisplay, before, {10, 10, 500, 400}, reason))
            return fail("separate audience display refused");
        if (audience::audiencePlacementAllowed(audienceDisplay, before, {900, 10, 500, 400}, reason))
            return fail("operator moved onto audience display without refusal");
        if (audience::audiencePlacementAllowed(audienceDisplay, before, {}, reason))
            return fail("missing operator accepted");
        auto after = before; after.remove(1);
        if (audience::sameDisplayLayout(before, after)) return fail("unplug topology considered unchanged");
        if (audience::audiencePlacementAllowed(audienceDisplay, after, {10, 10, 500, 400}, reason))
            return fail("disconnected audience display accepted");
        after = before; after.getReference(1).scale = 2.;
        if (audience::sameDisplayLayout(before, after)) return fail("DPI topology considered unchanged");
        if (!audience::sameDisplayLayout(before, before)) return fail("stable topology changed");
        if (!audience::audienceWindowContained(audienceDisplay, {800, 0, 800, 600}, {10, 10, 500, 400}))
            return fail("contained fullscreen audience refused");
        if (audience::audienceWindowContained(audienceDisplay, {750, 0, 800, 600}, {10, 10, 780, 400}))
            return fail("partially overlapping audience window accepted because its centre remained on target");
        return SelfTestResult{true, "separate output, operator move, unknown position, disconnect and DPI change"};
    });
}

}  // namespace ss::app
