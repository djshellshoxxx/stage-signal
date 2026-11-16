// Diagnostics tab (5.6), Show Doctor (10), report export (14). Extra files go in UI/Diagnostics/.
// Owner: ui-other. EMPTY panel in the contract commit: it only needs a UiModel&. Edit freely inside this file
// pair and inside your own subdirectory; do not edit PanelBase.h, Theme.h, TabHost.* or OperatorRoot.*.
#pragma once
#include "UI/PanelBase.h"

namespace ss::app {

class DiagnosticsPanel final : public PanelBase {
public:
    explicit DiagnosticsPanel(UiModel& model);
    ~DiagnosticsPanel() override;
    void resized() override;
private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DiagnosticsPanel)
};

}  // namespace ss::app
