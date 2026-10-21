#pragma once
#include "Plugin/PluginProcessor.h"

class StageSignalEditor final : public juce::AudioProcessorEditor {
public:
    explicit StageSignalEditor(StageSignalProcessor&);
    void paint(juce::Graphics&) override;
    void resized() override {}
private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StageSignalEditor)
};
