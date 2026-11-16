// Top status bar, 48 px (4.1). Extra files go in UI/Status/.
// Owner: ui-live. EMPTY panel in the contract commit: it only needs a UiModel&. Edit freely inside this file
// pair and inside your own subdirectory; do not edit PanelBase.h, Theme.h, TabHost.* or OperatorRoot.*.
#pragma once
#include "UI/PanelBase.h"

namespace ss::app {

class StatusBar final : public PanelBase {
public:
    explicit StatusBar(UiModel& model);
    ~StatusBar() override;
    void resized() override;
private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StatusBar)
};

}  // namespace ss::app
