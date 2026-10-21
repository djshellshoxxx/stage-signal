// Live tab (spec app-ui 5.1): scene pads, tempo/key, looks, preview, meters. Extra files go in UI/Live/.
// Owner: ui-live. EMPTY panel in the contract commit: it only needs a UiModel&. Edit freely inside this file
// pair and inside your own subdirectory; do not edit PanelBase.h, Theme.h, TabHost.* or OperatorRoot.*.
#pragma once
#include "UI/PanelBase.h"

namespace ss::app {

class LivePanel final : public PanelBase {
public:
    explicit LivePanel(UiModel& model);
    ~LivePanel() override;
    void resized() override;
private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LivePanel)
};

}  // namespace ss::app
