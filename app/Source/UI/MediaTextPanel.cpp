#include "UI/MediaTextPanel.h"
#include <initializer_list>
#include "UI/Overlays/OverlayText.h"
#include "UI/Theme.h"

namespace ss::app {

namespace {
void styleLabel(juce::Label& l, bool primary) {
    l.setFont(overlay::bodyFont(theme::kBodyFont));
    l.setColour(juce::Label::textColourId, primary ? theme::textPrimary() : theme::textSecondary());
}
}  // namespace

MediaTextPanel::MediaTextPanel(UiModel& model) : PanelBase(model, "Media / Text") {
    styleLabel(mediaTitle_, true);
    mediaTitle_.setText("Media", juce::dontSendNotification);
    styleLabel(mediaCount_, false);
    styleLabel(mediaNote_, false);
    mediaNote_.setText("Import is not in this build yet. Files are not read.", juce::dontSendNotification);
    import_.setEnabled(false);
    import_.setTooltip("Import needs the media worker, which is not in this build yet.");
    remove_.setEnabled(false);
    remove_.setTooltip("Nothing to remove yet.");

    styleLabel(textTitle_, true);
    textTitle_.setText("Text", juce::dontSendNotification);
    styleLabel(count_, false);
    draft_.setMultiLine(true);
    draft_.setReturnKeyStartsNewLine(true);
    draft_.setScrollbarsShown(true);
    draft_.setTextToShowWhenEmpty("Type the message here", theme::textMuted());
    draft_.setFont(overlay::bodyFont(theme::kBodyFont));
    draft_.setColour(juce::TextEditor::backgroundColourId, theme::bgSunken());
    draft_.setColour(juce::TextEditor::textColourId, theme::textPrimary());
    draft_.setColour(juce::TextEditor::outlineColourId, theme::lineControl());
    draft_.setInputRestrictions(kTextLimit);
    draft_.onTextChange = [this] {
        updateCount();
        if (previewing_) setPreview(false);   // an edit clears the preview until it is shown again
    };

    preview_.onClick = [this] { setPreview(!previewing_); };
    styleLabel(previewLabel_, false);
    previewLabel_.setVisible(false);
    publish_.setEnabled(false);
    publish_.setTooltip("Publishing a draft is not wired yet. Preview only shows the text.");
    styleLabel(publishNote_, false);
    publishNote_.setText("Publish sends the text to the audience outputs. It is not wired yet in this build.",
                         juce::dontSendNotification);
    clear_.onClick = [this] {
        status_.setText(juce::String(overlay::sendAction(model_, ss::ActionId::TextClear, 0.0, kAllOutputs,
                                                         &clearCorrelation_)),
                        juce::dontSendNotification);
    };
    clear_.setTooltip("Clears the published text on the selected outputs.");
    styleLabel(status_, false);
    updateCount();

    for (auto* c : std::initializer_list<juce::Component*>{&mediaTitle_, &mediaCount_, &mediaNote_, &import_, &remove_,
                                                           &textTitle_, &count_, &draft_, &preview_, &previewLabel_,
                                                           &publish_, &clear_, &publishNote_, &status_})
        addAndMakeVisible(c);
}

MediaTextPanel::~MediaTextPanel() = default;

void MediaTextPanel::paint(juce::Graphics& g) {
    g.fillAll(theme::bgPanel());
    g.setColour(theme::lineDivider());
    g.drawVerticalLine(300, 8.0f, static_cast<float>(getHeight() - 8));
}

void MediaTextPanel::updateCount() {
    const auto n = overlay::codePointCount(draft_.getText().toStdString());
    count_.setText(juce::String(static_cast<int>(n)) + " / " + juce::String(kTextLimit), juce::dontSendNotification);
}

void MediaTextPanel::setPreview(bool on) {
    previewing_ = on;
    preview_.setButtonText(on ? "Hide preview" : "Preview");
    if (on) {
        previewedText_ = draft_.getText().toStdString();
        previewLabel_.setText("PREVIEW\n" + juce::String(previewedText_), juce::dontSendNotification);
    }
    previewLabel_.setVisible(on);
    resized();
}

void MediaTextPanel::resized() {
    auto left = juce::Rectangle<int>(8, 8, 284, getHeight() - 16);
    mediaTitle_.setBounds(left.removeFromTop(22));
    mediaCount_.setBounds(left.removeFromTop(22));
    mediaNote_.setBounds(left.removeFromTop(36));
    auto lb = left.removeFromTop(30);
    import_.setBounds(lb.removeFromLeft(100).reduced(0, 2));
    lb.removeFromLeft(8);
    remove_.setBounds(lb.removeFromLeft(100).reduced(0, 2));

    auto r = juce::Rectangle<int>(312, 8, getWidth() - 320, getHeight() - 16);
    textTitle_.setBounds(r.removeFromTop(22));
    auto countRow = r.removeFromTop(20);
    count_.setBounds(countRow);
    r.removeFromTop(4);
    draft_.setBounds(r.removeFromTop(150));
    r.removeFromTop(6);
    auto buttons = r.removeFromTop(30);
    preview_.setBounds(buttons.removeFromLeft(110).reduced(0, 2));
    buttons.removeFromLeft(8);
    publish_.setBounds(buttons.removeFromLeft(100).reduced(0, 2));
    buttons.removeFromLeft(8);
    clear_.setBounds(buttons.removeFromLeft(150).reduced(0, 2));
    publishNote_.setBounds(r.removeFromTop(20));
    r.removeFromTop(4);
    if (previewing_) previewLabel_.setBounds(r.removeFromTop(juce::jmin(r.getHeight(), 60)));
    else previewLabel_.setBounds(juce::Rectangle<int>());
    status_.setBounds(r.removeFromTop(22));
}

void MediaTextPanel::uiSnapshotChanged(const UiSnapshot& s) {
    mediaCount_.setText(juce::String(static_cast<int>(s.media.loadedItems)) + " items loaded.", juce::dontSendNotification);
    if (!s.media.note.empty()) mediaNote_.setText(juce::String(s.media.note), juce::dontSendNotification);
}

void MediaTextPanel::uiActionOutcome(const ActionOutcome& o) {
    if (clearCorrelation_ == 0 || o.correlationId != clearCorrelation_) return;
    clearCorrelation_ = 0;
    if (!o.accepted) status_.setText(juce::String(o.reason.empty() ? "Not cleared. Try again." : o.reason),
                                     juce::dontSendNotification);
}

}  // namespace ss::app
