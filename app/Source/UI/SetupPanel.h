// Setup tab (spec app-ui 5.2) and the first-run wizard entry point (app-ui 9). Owner: ui-setup.
// Device choices go out as AppCommand through UiModel::postCommand; everything else is an action. The panel
// owns the audience window manager (Setup > Displays opens and closes audience windows).
#pragma once
#include <array>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/Audience/OutputManager.h"
#include "UI/PanelBase.h"
#include "UI/Setup/Wizard.h"

namespace ss::app {

class SetupPanel final : public PanelBase {
public:
    explicit SetupPanel(UiModel& model);
    ~SetupPanel() override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& key) override;

    // Starts the wizard at step 1 (a fresh run every time).
    void runWizard();
    bool wizardActive() const { return wizard_ != nullptr && wizard_->isVisible(); }
    setup::Wizard* wizard() { return wizard_.get(); }
    juce::Component* control(const juce::String& componentId);
    audience::OutputManager& outputs() { return outputs_; }

private:
    class Content;
    void uiSnapshotChanged(const UiSnapshot& s) override;
    void uiCommandOutcome(const CommandOutcome& o) override;

    std::unique_ptr<Content> content_;
    juce::Viewport viewport_;
    std::unique_ptr<setup::Wizard> wizard_;
    audience::OutputManager outputs_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SetupPanel)
};

}  // namespace ss::app
