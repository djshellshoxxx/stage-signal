// Media/Text tab (5.3), text draft/preview/publish (11). Extra files go in UI/MediaText/.
// Owner: ui-other. EMPTY panel in the contract commit: it only needs a UiModel&. Edit freely inside this file
// pair and inside your own subdirectory; do not edit PanelBase.h, Theme.h, TabHost.* or OperatorRoot.*.
#pragma once
#include "UI/PanelBase.h"

namespace ss::app {

class MediaTextPanel final : public PanelBase {
public:
    explicit MediaTextPanel(UiModel& model);
    ~MediaTextPanel() override;
    void resized() override;
private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MediaTextPanel)
};

}  // namespace ss::app
