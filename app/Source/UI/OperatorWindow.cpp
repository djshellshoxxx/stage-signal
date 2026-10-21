#include "UI/OperatorWindow.h"

namespace ss::app {

OperatorWindow::OperatorWindow(UiModel& model, std::function<void()> onCloseRequested)
    : juce::DocumentWindow("StageSignal", theme::bgNight(), juce::DocumentWindow::allButtons),
      onClose_(std::move(onCloseRequested)) {
    setUsingNativeTitleBar(true);
    setContentOwned(new OperatorRoot(model), true);
    setResizable(true, false);
    setResizeLimits(theme::kMinW, theme::kMinH, 4096, 4096);
    centreWithSize(theme::kDefaultW, theme::kDefaultH);
    setVisible(true);
}

}  // namespace ss::app
