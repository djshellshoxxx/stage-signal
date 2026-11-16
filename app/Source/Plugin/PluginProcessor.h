#pragma once
#include <memory>
#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>
#include "App/AppController.h"
#include "App/HostMode.h"

// The single juce::AudioProcessor shared by standalone and CLAP (APP-SH-14). Owns nothing heavy: it holds a
// shared_ptr to the process-wide AppController and a raw pointer to its AudioIngress for the audio thread.
class StageSignalProcessor final : public juce::AudioProcessor {
public:
    StageSignalProcessor();
    ~StageSignalProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "StageSignal"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    // Versioned, bounded state blob (app-shell 7.3, APP-SH-09) is a shell-integration follow-up.
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // Lazily creates and starts the process-wide controller (message thread).
    std::shared_ptr<ss::app::AppController> ensureController();
    const ss::app::HostMode& hostMode() const { return *mode_; }

private:
    const ss::app::HostMode* mode_;
    std::shared_ptr<ss::app::AppController> controller_;
    ss::app::AudioIngress* ingress_ = nullptr;     // read on the audio thread; assigned only in prepareToPlay/release
    const ss::IClock* clock_ = nullptr;
    std::vector<float> scratch_;                   // interleave buffer, sized in prepareToPlay
    double rate_ = 48000.0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StageSignalProcessor)
};
