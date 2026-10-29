// Record/Broadcast tab (app-ui 5.5). Owner: ui-other. Start and stop go through UiModel::submitAction; folder and
// output choices through UiModel::postCommand. Start broadcast opens an inline confirmation with the publication
// summary. The stream key is masked and is never read back into the UI after it is typed.
#pragma once
#include <cstdint>
#include <string>
#include "UI/Overlays/FolderPicker.h"
#include "UI/PanelBase.h"

namespace ss::app {

class RecordBroadcastPanel final : public PanelBase {
public:
    explicit RecordBroadcastPanel(UiModel& model);
    ~RecordBroadcastPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void uiSnapshotChanged(const UiSnapshot& s) override;
    void uiActionOutcome(const ActionOutcome& o) override;
    void uiCommandOutcome(const CommandOutcome& o) override;
    void showBroadcastConfirm(bool show);
    std::string publicationSummary(const UiSnapshot& s) const;

    FolderPicker folderPicker_;
    std::uint64_t recCorrelation_ = 0, castCorrelation_ = 0, folderCorrelation_ = 0;
    bool castConfirmOpen_ = false;
    bool recordingActive_ = false, broadcastActive_ = false;

    // Recording column
    juce::Label recTitle_, recChip_, recSource_, recFormat_, recFolder_, recElapsed_, recNote_, recStatus_;
    juce::TextButton chooseFolder_{"Choose folder"}, startRec_{"Start recording"}, stopRec_{"Stop recording"},
        marker_{"Add marker"};

    // Broadcast column
    juce::Label castTitle_, castChip_, hostLabel_, keyLabel_, keyNote_, summary_, castNote_, castStats_, castStatus_,
        confirmText_, settingsNote_;
    juce::TextEditor host_, port_, path_, key_;
    juce::TextButton saveSettings_{"Save settings"}, startCast_{"Start broadcast"}, stopCast_{"Stop broadcast"},
        confirmStart_{"Start now"}, cancelStart_{"Cancel"};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecordBroadcastPanel)
};

}  // namespace ss::app
