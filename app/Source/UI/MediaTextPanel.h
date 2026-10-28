// Media/Text tab (app-ui 5.3 and 11). Owner: ui-other. Media import needs the media worker (not in this build), so the
// list shows what the UI model reports. The text draft, preview and character count are local. Publishing a draft
// is held back until the text reaches the engine with the action (see the note under the editor).
#pragma once
#include <string>
#include <memory>
#include "UI/PanelBase.h"

namespace ss::app {

class MediaTextPanel final : public PanelBase {
public:
    explicit MediaTextPanel(UiModel& model);
    ~MediaTextPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    static constexpr int kTextLimit = 4096;   // code points (app-ui 11)

private:
    void uiSnapshotChanged(const UiSnapshot& s) override;
    void uiActionOutcome(const ActionOutcome& o) override;
    void uiCommandOutcome(const CommandOutcome& o) override;
    void updateCount();
    void setPreview(bool on);

    juce::Label mediaTitle_, mediaCount_, mediaNote_, textTitle_, count_, previewLabel_, publishNote_, status_;
    juce::ComboBox clipSlot_;
    juce::Slider clipSpeed_;
    juce::ToggleButton clipLoop_{"Loop"}, clipPause_{"Pause clip"};
    juce::TextButton clipApply_{"Apply playback"};
    juce::Label clipStatus_;
    juce::Label cameraTitle_, cameraStatus_;
    juce::ComboBox cameraDevices_;
    juce::TextButton cameraRefresh_{"Refresh"}, cameraOpen_{"Preview camera"}, cameraClose_{"Close camera"};
    juce::TextButton cameraPublish_{"Publish to Output 1"}, cameraUnpublish_{"Unpublish camera"};
    juce::ImageComponent cameraImage_;
    std::vector<CameraDesc> cameras_;
    std::shared_ptr<const PixelBuffer> cameraPreview_;
    juce::TextButton import_{"Import..."}, remove_{"Remove"};
    juce::TextEditor draft_;
    juce::TextButton preview_{"Preview"}, publish_{"Publish"}, clear_{"Clear published"};
    std::uint64_t clearCorrelation_ = 0;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::uint64_t cmdCorrelation_ = 0;
    int imported_ = 0;
    bool previewing_ = false;
    std::string previewedText_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MediaTextPanel)
};

}  // namespace ss::app
