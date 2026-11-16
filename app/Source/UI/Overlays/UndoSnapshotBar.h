// Undo/Redo and A/B snapshot controls (app-ui 13, Live tab "Edit/snapshots" block). Owner: ui-other.
// Only the controls live here: the undo stack and snapshot store belong to `show`. Actions 100 to 105.
// Embed it in the Live tab; it follows the snapshot itself (UiModelListener), no timers.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModel.h"

namespace ss::app {

class UndoSnapshotBar final : public juce::Component, private UiModelListener {
public:
    explicit UndoSnapshotBar(UiModel& model);
    ~UndoSnapshotBar() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    // Updates enabled states and the A/B slot words from the snapshot. Does not allocate.
    void observe(const UiSnapshot& s);
    void uiSnapshotChanged(const UiSnapshot& s) override { observe(s); }

    // Fade time for recalls, seconds. Accepted range 0 to 10 (app-ui 13); anything else is refused, not clamped.
    static bool parseFade(const std::string& text, double& seconds);

    juce::String slotWordA() const { return slotA_.getText(); }
    juce::String slotWordB() const { return slotB_.getText(); }

private:
    void send(ss::ActionId id, double value = 0.0);
    void recall(ss::ActionId id);

    UiModel& model_;
    juce::TextButton undo_{"Undo"}, redo_{"Redo"};
    juce::TextButton storeA_{"A store"}, recallA_{"A recall"}, storeB_{"B store"}, recallB_{"B recall"};
    juce::Label fadeLabel_, slotA_, slotB_, status_;
    juce::TextEditor fade_;
    bool slotAUsed_ = false, slotBUsed_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UndoSnapshotBar)
};

}  // namespace ss::app
