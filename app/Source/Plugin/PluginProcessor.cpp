#include "Plugin/PluginProcessor.h"
#include "Plugin/PluginEditor.h"

StageSignalProcessor::StageSignalProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}

void StageSignalProcessor::prepareToPlay(double, int) {}

bool StageSignalProcessor::isBusesLayoutSupported(const BusesLayout& l) const {
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo()) && in == out;
}

void StageSignalProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    // Standalone: monitoring is off by default (spec 12: no feedback into the PA). Plugin: pass through.
    if (wrapperType == wrapperType_Standalone) buffer.clear();
}

juce::AudioProcessorEditor* StageSignalProcessor::createEditor() { return new StageSignalEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new StageSignalProcessor(); }
