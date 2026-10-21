// Bottom strip, 60 px, blackout and emergency controls (4.2). Extra files go in UI/Strip/.
// Owner: ui-live. EMPTY panel in the contract commit: it only needs a UiModel&. Edit freely inside this file
// pair and inside your own subdirectory; do not edit PanelBase.h, Theme.h, TabHost.* or OperatorRoot.*.
#pragma once
#include "UI/PanelBase.h"

namespace ss::app {

class PerformanceStrip final : public PanelBase {
public:
    explicit PerformanceStrip(UiModel& model);
    ~PerformanceStrip() override;
    void resized() override;
private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PerformanceStrip)
};

}  // namespace ss::app
