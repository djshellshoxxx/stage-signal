#include "UI/OperatorRoot.h"
#include "UI/Live/Shortcuts.h"
#include "UI/Setup/WizardProgress.h"

namespace ss::app {

OperatorRoot::OperatorRoot(UiModel& model)
    : model_(model), status_(model), tabs_(model), strip_(model), poller_(model), palette_(model) {
    addAndMakeVisible(status_);
    addAndMakeVisible(tabs_);
    addAndMakeVisible(strip_);
    addChildComponent(banner_);
    for (auto* b : {&restoreAuto_, &openLast_, &startEmpty_}) addChildComponent(b);
    banner_.setColour(juce::Label::textColourId, theme::stateWarn());
    banner_.setFont(juce::FontOptions(theme::kBodyFont));
    restoreAuto_.onClick = [this] { sendRecovery(AppCommandKind::RecoveryRestoreAutosave); };
    openLast_.onClick = [this] { sendRecovery(AppCommandKind::RecoveryOpenLast); };
    startEmpty_.onClick = [this] { sendRecovery(AppCommandKind::RecoveryStartEmpty); };
    status_.onJumpToTab = [this](int i) { tabs_.setCurrentTabIndex(i); };
    model_.addListener(this);
    setWantsKeyboardFocus(true);
    setSize(theme::kDefaultW, theme::kDefaultH);
    if (auto snapshot = model_.snapshot()) uiSnapshotChanged(*snapshot);
}

OperatorRoot::~OperatorRoot() { model_.removeListener(this); }

void OperatorRoot::sendRecovery(AppCommandKind k) {
    AppCommand c;
    c.kind = k;
    model_.postCommand(c);
}

void OperatorRoot::uiSnapshotChanged(const UiSnapshot& s) {
    if (!wizardOffered_ && !s.headless && s.host == HostKind::Standalone && !s.recovery && !s.secondary) {
        wizardOffered_ = true;
        setup::WizardProgress progress;
        bool completed = false;
        for (const auto& pref : s.prefs) if (pref.first == "wizard.progress" && setup::WizardProgress::decode(pref.second, progress)) completed = progress.completed;
        if (!completed) { tabs_.showTab(Tab::Setup); tabs_.setup().runWizard(); }
    }
    const bool rec = s.recoveryInfo.active, sec = s.secondary;
    const bool want = rec || sec;
    juce::String text;
    if (rec) text = juce::String(s.recoveryInfo.text);
    else if (sec) text = "Another StageSignal in this host owns the outputs. This one shows analysis only. Lighting, MIDI output, recording and broadcast are off.";
    if (banner_.getText() != text) banner_.setText(text, juce::dontSendNotification);
    const bool buttons = rec;
    if (want != bannerWanted_ || buttons != recoveryButtons_) {
        bannerWanted_ = want; recoveryButtons_ = buttons;
        banner_.setVisible(want);
        restoreAuto_.setVisible(buttons && s.recoveryInfo.autosaveAvailable);
        openLast_.setVisible(buttons && s.recoveryInfo.lastSavedAvailable);
        startEmpty_.setVisible(buttons);
        resized();
    }
}

bool OperatorRoot::keyPressed(const juce::KeyPress& key) {
    const auto m = key.getModifiers();
    if ((m.isCtrlDown() || m.isCommandDown()) && (key.getKeyCode() == 'K' || key.getKeyCode() == 'k')) {
        if (!palette_.isOpen()) palette_.open(*this);
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::F1Key && key.getModifiers().isAnyModifierKeyDown() == false) { tabs_.showTab(Tab::Help); return true; }
    return live::handleShortcut(model_, key);
}

void OperatorRoot::paint(juce::Graphics& g) { g.fillAll(theme::bgNight()); }

void OperatorRoot::resized() {
    auto r = getLocalBounds();
    compact_ = theme::isCompact(r.getWidth(), r.getHeight());
    status_.setBounds(r.removeFromTop(theme::kStatusBarH));
    strip_.setBounds(r.removeFromBottom(theme::kStripH));
    if (bannerWanted_) {
        auto b = r.removeFromTop(compact_ ? 56 : 32).reduced(8, 2);
        if (recoveryButtons_) {
            startEmpty_.setBounds(b.removeFromRight(110)); b.removeFromRight(4);
            openLast_.setBounds(b.removeFromRight(120)); b.removeFromRight(4);
            restoreAuto_.setBounds(b.removeFromRight(130)); b.removeFromRight(8);
        }
        banner_.setBounds(b);
        banner_.setMinimumHorizontalScale(0.8f);
    }
    tabs_.setBounds(r);          // tab bar (36) + page area (456 at 800x600)
}

}  // namespace ss::app
