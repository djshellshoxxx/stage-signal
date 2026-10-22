// Live tab (spec app-ui 5.1): scene pads, bank, tempo/key, mode and cues, energy arc, looks, effects, snapshots,
// preview. Extra files go in UI/Live/. Owner: ui-live.
// Reads state only from UiSnapshot; changes state only through UiModel::submitAction (uikit::sendAction).
#pragma once
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/PanelBase.h"
#include "UI/Live/EnergyArcMeter.h"
#include "UI/Live/LivePreview.h"
#include "UI/Live/ScenePad.h"

namespace ss::app {

class LivePanel final : public PanelBase {
public:
    explicit LivePanel(UiModel& model);
    ~LivePanel() override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& key) override;

    // Selected target (kAllOutputs or an output id) and the controls that tests drive.
    OutputId target() const;
    juce::Component* control(const juce::String& componentId);
    juce::String noticeText() const;

private:
    class Content;
    void uiSnapshotChanged(const UiSnapshot& s) override;
    void uiActionOutcome(const ActionOutcome& o) override;

    std::unique_ptr<Content> content_;
    juce::Viewport viewport_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LivePanel)
};

}  // namespace ss::app
