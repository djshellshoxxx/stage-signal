// Help tab (5.7), keyboard help from palette core. Extra files go in UI/Help/.
// Owner: ui-other. EMPTY panel in the contract commit: it only needs a UiModel&. Edit freely inside this file
// pair and inside your own subdirectory; do not edit PanelBase.h, Theme.h, TabHost.* or OperatorRoot.*.
#pragma once
#include "UI/PanelBase.h"

namespace ss::app {

class HelpPanel final : public PanelBase {
public:
    explicit HelpPanel(UiModel& model);
    ~HelpPanel() override;
    void resized() override;
private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HelpPanel)
};

}  // namespace ss::app
