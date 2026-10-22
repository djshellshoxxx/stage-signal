// Bottom performance strip, 60 px (app-ui 4.2): blackout latch controls, pause/next, brightness and intensity.
// Always visible on every tab. Owner: ui-live. Extra files go in UI/Strip/.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/PanelBase.h"

namespace ss::app {

class PerformanceStrip final : public PanelBase {
public:
    explicit PerformanceStrip(UiModel& model);
    ~PerformanceStrip() override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& key) override;

    // Visible text of the blackout buttons (tests check the latched words).
    juce::String visualText() const { return visual_.getButtonText(); }
    juce::String lightingText() const { return lighting_.getButtonText(); }
    bool restoreEnabled() const { return restore_.isEnabled(); }

private:
    void uiSnapshotChanged(const UiSnapshot& s) override;
    void uiActionOutcome(const ActionOutcome& o) override;
    void announce(const char* text);

    juce::TextButton visual_, lighting_, emergency_, restore_, pause_, resume_, next_;
    juce::Slider brightness_, intensity_;
    bool lastVisual_ = false, lastLighting_ = false, lastEmergency_ = false, syncing_ = false;
    bool seenFirst_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PerformanceStrip)
};

}  // namespace ss::app
