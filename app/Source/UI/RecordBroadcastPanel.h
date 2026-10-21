// Record/Broadcast tab (5.5). Extra files go in UI/RecordBroadcast/.
// Owner: ui-other. EMPTY panel in the contract commit: it only needs a UiModel&. Edit freely inside this file
// pair and inside your own subdirectory; do not edit PanelBase.h, Theme.h, TabHost.* or OperatorRoot.*.
#pragma once
#include "UI/PanelBase.h"

namespace ss::app {

class RecordBroadcastPanel final : public PanelBase {
public:
    explicit RecordBroadcastPanel(UiModel& model);
    ~RecordBroadcastPanel() override;
    void resized() override;
private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecordBroadcastPanel)
};

}  // namespace ss::app
