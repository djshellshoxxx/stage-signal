// Standalone operator window (app-ui 2). Owner: shell-integration. Resizable, min 640x480, native title bar.
// Window position/size persistence per display identity is a follow-up (needs the settings store).
#pragma once
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/OperatorRoot.h"

namespace ss::app {

class OperatorWindow final : public juce::DocumentWindow, private juce::Timer {
public:
    OperatorWindow(UiModel& model, std::function<void()> onCloseRequested);
    ~OperatorWindow() override;
    void moved() override;
    void resized() override;
    void closeButtonPressed() override { if (onClose_) onClose_(); }
private:
    void timerCallback() override;
    UiModel& model_;
    bool restoring_ = true;
    std::function<void()> onClose_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OperatorWindow)
};

}  // namespace ss::app
