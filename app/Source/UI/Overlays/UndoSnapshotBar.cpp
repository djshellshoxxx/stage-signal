#include "UI/Overlays/UndoSnapshotBar.h"
#include <cstdlib>
#include "UI/Overlays/OverlayText.h"
#include "UI/Theme.h"

namespace ss::app {

bool UndoSnapshotBar::parseFade(const std::string& text, double& seconds) {
    if (text.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return false;
    if (!(v >= 0.0 && v <= 10.0)) return false;
    seconds = v;
    return true;
}

UndoSnapshotBar::UndoSnapshotBar(UiModel& model) : model_(model) {
    undo_.onClick = [this] { send(ss::ActionId::Undo); };
    redo_.onClick = [this] { send(ss::ActionId::Redo); };
    storeA_.onClick = [this] { send(ss::ActionId::SnapshotStoreA); };
    storeB_.onClick = [this] { send(ss::ActionId::SnapshotStoreB); };
    recallA_.onClick = [this] { recall(ss::ActionId::SnapshotRecallA); };
    recallB_.onClick = [this] { recall(ss::ActionId::SnapshotRecallB); };

    fadeLabel_.setText("Fade (s)", juce::dontSendNotification);
    fade_.setText("1.0", false);
    fade_.setInputRestrictions(5, "0123456789.");
    fade_.setColour(juce::TextEditor::backgroundColourId, theme::bgSunken());
    fade_.setColour(juce::TextEditor::textColourId, theme::textPrimary());
    fade_.setColour(juce::TextEditor::outlineColourId, theme::lineControl());
    fade_.setTooltip("Crossfade time for snapshot recall, 0 to 10 seconds. 0 cuts straight to the look.");

    for (auto* l : {&fadeLabel_, &slotA_, &slotB_, &status_}) {
        l->setFont(overlay::bodyFont(theme::kBodyFont));
        l->setColour(juce::Label::textColourId, theme::textSecondary());
    }
    slotA_.setText("A empty", juce::dontSendNotification);
    slotB_.setText("B empty", juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*>{&undo_, &redo_, &storeA_, &recallA_, &storeB_, &recallB_,
                                                           &fadeLabel_, &fade_, &slotA_, &slotB_, &status_})
        addAndMakeVisible(c);
    model_.addListener(this);
}

UndoSnapshotBar::~UndoSnapshotBar() { model_.removeListener(this); }

void UndoSnapshotBar::paint(juce::Graphics& g) { g.fillAll(theme::bgPanel()); }

void UndoSnapshotBar::resized() {
    auto r = getLocalBounds().reduced(4, 2);
    auto top = r.removeFromTop(30);
    undo_.setBounds(top.removeFromLeft(72).reduced(2, 0));
    redo_.setBounds(top.removeFromLeft(72).reduced(2, 0));
    top.removeFromLeft(12);
    slotA_.setBounds(top.removeFromLeft(74));
    storeA_.setBounds(top.removeFromLeft(72).reduced(2, 0));
    recallA_.setBounds(top.removeFromLeft(72).reduced(2, 0));
    top.removeFromLeft(8);
    slotB_.setBounds(top.removeFromLeft(74));
    storeB_.setBounds(top.removeFromLeft(72).reduced(2, 0));
    recallB_.setBounds(top.removeFromLeft(72).reduced(2, 0));
    auto bottom = r.removeFromTop(26);
    fadeLabel_.setBounds(bottom.removeFromLeft(72));
    fade_.setBounds(bottom.removeFromLeft(64).reduced(0, 1));
    status_.setBounds(bottom.reduced(8, 0));
}

void UndoSnapshotBar::observe(const UiSnapshot& s) {
    const auto& show = s.show;
    undo_.setEnabled(show.undoDepth > 0);
    undo_.setTooltip(show.undoDepth > 0 ? "Undo the last edit." : "Nothing to undo.");
    redo_.setEnabled(show.redoDepth > 0);
    redo_.setTooltip(show.redoDepth > 0 ? "Redo the edit you undid." : "Nothing to redo.");
    slotAUsed_ = show.slots[0].used;
    slotBUsed_ = show.slots[1].used;
    slotA_.setText(slotAUsed_ ? "A stored" : "A empty", juce::dontSendNotification);
    slotB_.setText(slotBUsed_ ? "B stored" : "B empty", juce::dontSendNotification);
    recallA_.setEnabled(slotAUsed_);
    recallB_.setEnabled(slotBUsed_);
    recallA_.setTooltip(slotAUsed_ ? "Switch to the look saved in A." : "Store a look in A first.");
    recallB_.setTooltip(slotBUsed_ ? "Switch to the look saved in B." : "Store a look in B first.");
}

void UndoSnapshotBar::send(ss::ActionId id, double value) {
    status_.setText(juce::String(overlay::sendAction(model_, id, value, kAllOutputs, nullptr)), juce::dontSendNotification);
}

void UndoSnapshotBar::recall(ss::ActionId id) {
    double seconds = 0.0;
    if (!parseFade(fade_.getText().toStdString(), seconds)) {
        status_.setText("Fade must be 0 to 10 seconds.", juce::dontSendNotification);
        return;
    }
    send(id, seconds);
}

}  // namespace ss::app
