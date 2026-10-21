// MIDI/Lighting tab (app-ui 5.4). Owner: ui-other. Reads UiSnapshot (midi, lighting); writes through
// UiModel::submitAction (arm, disarm, dry run, blackout) and UiModel::postCommand (ports, interface, destination).
// Arm opens an inline confirmation, never a blocking dialog. Parts the UI model does not report yet are shown as such.
#pragma once
#include <cstdint>
#include <string>
#include "UI/PanelBase.h"

namespace ss::app {

class MidiLightingPanel final : public PanelBase {
public:
    explicit MidiLightingPanel(UiModel& model);
    ~MidiLightingPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void uiSnapshotChanged(const UiSnapshot& s) override;
    void uiActionOutcome(const ActionOutcome& o) override;
    void uiCommandOutcome(const CommandOutcome& o) override;
    void showArmConfirm(bool show);
    void setLightingStatus(const juce::String& text);

    // MIDI column
    juce::Label midiTitle_, portsText_, learnStatus_, mappingHeader_, mappingEmpty_, alarmsText_, midiNote_;
    juce::TextButton learn_{"Learn"};
    juce::ToggleButton clock_{"Send MIDI clock (opt-in)"}, sysEx_{"SysEx (off, up to 1024 bytes)"},
        midiOut_{"MIDI output enabled"};
    bool learning_ = false;

    // Lighting column
    juce::Label lightTitle_, stateChip_, lightNote_, destLabel_, universeText_, lightStatus_,
        confirmText_, patchText_;
    juce::TextEditor interfaceIp_, destIp_, destPort_;
    juce::TextButton useInterface_{"Use interface"}, setDest_{"Set destination"};
    juce::ToggleButton broadcast_{"Broadcast to every device on this network"};
    juce::TextButton arm_{"Arm"}, disarm_{"Disarm"}, confirmArm_{"Arm now"}, cancelArm_{"Cancel"};
    juce::ToggleButton dryRun_{"Dry run (compute, do not send)"};
    juce::TextButton blackout_{"Lighting blackout"}, restore_{"Restore"};
    bool armConfirmOpen_ = false;
    std::uint64_t lastActionCorrelation_ = 0, lastCommandCorrelation_ = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiLightingPanel)
};

}  // namespace ss::app
