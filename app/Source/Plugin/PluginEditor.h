#pragma once
#include <memory>
#include "Plugin/PluginProcessor.h"
#include "UI/OperatorRoot.h"

// Embedded operator UI: a thin host for OperatorRoot (same root component as the standalone window).
// The editor is a view; anything that must keep running lives in AppController (app-shell 7.2).
class StageSignalEditor final : public juce::AudioProcessorEditor {
public:
    explicit StageSignalEditor(StageSignalProcessor&);
    ~StageSignalEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    std::shared_ptr<ss::app::AppController> controller_;
    std::unique_ptr<ss::app::OperatorRoot> root_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StageSignalEditor)
};
