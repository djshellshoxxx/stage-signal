// MIDI/Lighting tab (5.4): mapping, learn, patch, Arm. Extra files go in UI/MidiLighting/.
// Owner: ui-other. EMPTY panel in the contract commit: it only needs a UiModel&. Edit freely inside this file
// pair and inside your own subdirectory; do not edit PanelBase.h, Theme.h, TabHost.* or OperatorRoot.*.
#pragma once
#include "UI/PanelBase.h"

namespace ss::app {

class MidiLightingPanel final : public PanelBase {
public:
    explicit MidiLightingPanel(UiModel& model);
    ~MidiLightingPanel() override;
    void resized() override;
private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiLightingPanel)
};

}  // namespace ss::app
