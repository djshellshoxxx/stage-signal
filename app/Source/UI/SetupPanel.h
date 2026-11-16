// Setup tab (5.2) and the setup wizard (9). Extra files go in UI/Setup/.
// Owner: ui-setup. EMPTY panel in the contract commit: it only needs a UiModel&. Edit freely inside this file
// pair and inside your own subdirectory; do not edit PanelBase.h, Theme.h, TabHost.* or OperatorRoot.*.
#pragma once
#include "UI/PanelBase.h"

namespace ss::app {

class SetupPanel final : public PanelBase {
public:
    explicit SetupPanel(UiModel& model);
    ~SetupPanel() override;
    void resized() override;
private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SetupPanel)
};

}  // namespace ss::app
