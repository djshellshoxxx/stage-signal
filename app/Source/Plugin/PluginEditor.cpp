#include "Plugin/PluginEditor.h"

StageSignalEditor::StageSignalEditor(StageSignalProcessor& p) : AudioProcessorEditor(p) { setSize(800, 600); }

void StageSignalEditor::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff04060b));
    g.setColour(juce::Colours::white);
    g.setFont(24.0f);
    g.drawText("StageSignal", getLocalBounds(), juce::Justification::centred);
}
