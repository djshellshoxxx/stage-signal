#include "Plugin/PluginEditor.h"

StageSignalEditor::StageSignalEditor(StageSignalProcessor& p) : AudioProcessorEditor(p) {
    controller_ = p.ensureController();
    root_ = std::make_unique<ss::app::OperatorRoot>(controller_->ui());
    addAndMakeVisible(*root_);
    setResizable(true, false);
    setResizeLimits(ss::app::theme::kMinW, ss::app::theme::kMinH, 4096, 4096);
    setSize(ss::app::theme::kDefaultW, ss::app::theme::kDefaultH);
}

StageSignalEditor::~StageSignalEditor() = default;

void StageSignalEditor::paint(juce::Graphics& g) { g.fillAll(ss::app::theme::bgNight()); }

void StageSignalEditor::resized() {
    if (root_) root_->setBounds(getLocalBounds());
}
