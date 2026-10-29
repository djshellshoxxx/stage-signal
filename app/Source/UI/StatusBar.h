// Top status bar, 48 px (app-ui 4.1). Chips are read-only; Enter or a click on a chip jumps to its tab.
// Owner: ui-live. Extra files go in UI/Status/.
#pragma once
#include <array>
#include <functional>
#include "UI/PanelBase.h"
#include "UI/Status/StatusChip.h"

namespace ss::app {

class StatusBar final : public PanelBase {
public:
    static constexpr int kChipCount = 10;
    // Tab index to jump to when a chip is activated (TabHost order: Live 0, Setup 1, MediaText 2,
    // MidiLighting 3, RecordBroadcast 4, Diagnostics 5, Help 6). OperatorRoot wires this to TabHost::showTab.
    std::function<void(int tabIndex)> onJumpToTab;

    explicit StatusBar(UiModel& model);
    ~StatusBar() override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& key) override;

    const status::Chip& chip(int index) const { return chips_[static_cast<std::size_t>(index)].chip(); }
    int chipCount() const { return kChipCount; }

private:
    void uiSnapshotChanged(const UiSnapshot& s) override;
    void announceTransitions(const UiSnapshot& s);

    std::array<status::StatusChip, kChipCount> chips_;
    std::array<int, kChipCount> tabFor_{};
    int lastLighting_ = -1, lastRecord_ = -1, lastBroadcast_ = -1, lastAudio_ = -1;
    bool compact_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StatusBar)
};

}  // namespace ss::app
