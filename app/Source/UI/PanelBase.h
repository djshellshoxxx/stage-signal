// Common base for tab panels, status bar and strip. Owner: shell-integration (do not edit from ui-* workers).
// Registers as a UiModelListener so a panel is repainted when a new snapshot arrives; override
// uiSnapshotChanged() to update widgets. Panels only talk to the UiModel (submitAction / postCommand).
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModel.h"
#include "UI/Theme.h"

namespace ss::app {

class PanelBase : public juce::Component, protected UiModelListener {
public:
    explicit PanelBase(UiModel& m, juce::String title) : model_(m), title_(std::move(title)) { model_.addListener(this); }
    ~PanelBase() override { model_.removeListener(this); }

    const juce::String& title() const { return title_; }

    void paint(juce::Graphics& g) override {
        g.fillAll(theme::bgNight());
        g.setColour(theme::textMuted());
        g.setFont(theme::kBodyFont);
        g.drawText(title_ + " (empty panel, owned by its UI worker)", getLocalBounds(), juce::Justification::centred);
    }

protected:
    void uiSnapshotChanged(const UiSnapshot&) override { repaint(); }
    UiModel& model_;
private:
    juce::String title_;
};

}  // namespace ss::app
