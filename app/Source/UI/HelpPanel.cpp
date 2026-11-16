#include "UI/HelpPanel.h"
#include <memory>
#include "UI/Overlays/OverlayText.h"
#include "UI/Theme.h"

namespace ss::app {

class HelpPanel::TopicModel final : public juce::ListBoxModel {
public:
    explicit TopicModel(HelpPanel& owner) : owner_(owner) {}
    int getNumRows() override { return owner_.topicCount(); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
        if (row < 0 || row >= owner_.topicCount()) return;
        g.fillAll(selected ? theme::bgRaised() : theme::bgPanel());
        g.setFont(overlay::bodyFont(theme::kBodyFont));
        g.setColour(selected ? theme::textPrimary() : theme::textSecondary());
        g.drawText(juce::String(owner_.topics_[static_cast<std::size_t>(row)].title), 8, 0, w - 16, h,
                   juce::Justification::centredLeft, true);
    }
    void selectedRowsChanged(int last) override { owner_.showTopic(last); }
private:
    HelpPanel& owner_;
};

HelpPanel::HelpPanel(UiModel& model)
    : PanelBase(model, "Help"), topics_(help::topics(palette::KeyMap::defaults())), model_list_(std::make_unique<TopicModel>(*this)) {
    list_.setModel(model_list_.get());
    list_.setRowHeight(26);
    list_.setColour(juce::ListBox::backgroundColourId, theme::bgPanel());
    list_.setMultipleSelectionEnabled(false);
    text_.setMultiLine(true);
    text_.setReadOnly(true);
    text_.setScrollbarsShown(true);
    text_.setCaretVisible(false);
    text_.setFont(overlay::bodyFont(theme::kBodyFont));
    text_.setColour(juce::TextEditor::backgroundColourId, theme::bgSunken());
    text_.setColour(juce::TextEditor::textColourId, theme::textPrimary());
    text_.setColour(juce::TextEditor::outlineColourId, theme::lineDivider());
    addAndMakeVisible(list_);
    addAndMakeVisible(text_);
    list_.selectRow(0);
    showTopic(0);
}

HelpPanel::~HelpPanel() { list_.setModel(nullptr); }

void HelpPanel::paint(juce::Graphics& g) { g.fillAll(theme::bgPanel()); }

void HelpPanel::resized() {
    auto r = getLocalBounds().reduced(8, 8);
    list_.setBounds(r.removeFromLeft(210));
    r.removeFromLeft(10);
    text_.setBounds(r);
}

std::vector<std::string> HelpPanel::titles() const {
    std::vector<std::string> out;
    for (const auto& t : topics_) out.push_back(t.title);
    return out;
}

void HelpPanel::showTopic(int index) {
    if (index < 0 || index >= topicCount()) return;
    text_.setText(juce::String(topics_[static_cast<std::size_t>(index)].body), false);
    text_.moveCaretToTop(false);
}

}  // namespace ss::app
