#include "UI/OperatorWindow.h"

namespace ss::app {

OperatorWindow::OperatorWindow(UiModel& model, std::function<void()> onCloseRequested)
    : juce::DocumentWindow("StageSignal", theme::bgNight(), juce::DocumentWindow::allButtons),
      model_(model), onClose_(std::move(onCloseRequested)) {
    setUsingNativeTitleBar(true);
    setContentOwned(new OperatorRoot(model), true);
    setResizable(true, false);
    setResizeLimits(theme::kMinW, theme::kMinH, 4096, 4096);
    centreWithSize(theme::kDefaultW, theme::kDefaultH);
    if (auto snapshot = model_.snapshot()) for (const auto& pref : snapshot->prefs) {
        if (pref.first != "ui.operator.bounds" || pref.second.size() > 64) continue;
        const auto bounds = juce::Rectangle<int>::fromString(pref.second);
        if (bounds.getWidth() < theme::kMinW || bounds.getHeight() < theme::kMinH || bounds.getWidth() > 4096 || bounds.getHeight() > 4096) continue;
        for (const auto& display : juce::Desktop::getInstance().getDisplays().displays)
            if (display.userBounds.toNearestInt().contains(bounds)) { setBounds(bounds); break; }
    }
    setVisible(true);
    restoring_ = false;
}

OperatorWindow::~OperatorWindow() { stopTimer(); }
void OperatorWindow::moved() { if (!restoring_) startTimer(250); }
void OperatorWindow::resized() {
    juce::DocumentWindow::resized();
    if (!restoring_) startTimer(250);
}
void OperatorWindow::timerCallback() {
    AppCommand command; command.kind = AppCommandKind::SetPref; command.text = "ui.operator.bounds";
    command.text2 = getBounds().toString().toStdString();
    if (model_.postCommand(command)) stopTimer();
}

}  // namespace ss::app
