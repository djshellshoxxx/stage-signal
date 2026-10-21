#include "Plugin/PluginProcessor.h"
#include <algorithm>
#include "App/ControllerHost.h"
#include "Plugin/PluginEditor.h"

namespace {
// Standalone reports wrapperType_Standalone. CLAP (clap-juce-extensions) reports wrapperType_Undefined, so
// "anything else" is treated as CLAP (app-shell 7.1). Refine with clap_properties::is_clap if that ever matters.
ss::app::HostKind hostKindFor(juce::AudioProcessor::WrapperType t) {
    return t == juce::AudioProcessor::wrapperType_Standalone ? ss::app::HostKind::Standalone : ss::app::HostKind::Clap;
}
}  // namespace

StageSignalProcessor::StageSignalProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      mode_(&ss::app::hostModeFor(hostKindFor(wrapperType))) {}

StageSignalProcessor::~StageSignalProcessor() = default;

std::shared_ptr<ss::app::AppController> StageSignalProcessor::ensureController() {
    if (!controller_) controller_ = ss::app::ControllerHost::acquire(mode_->kind());
    return controller_;
}

void StageSignalProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    auto c = ensureController();
    rate_ = sampleRate;
    const int ch = std::max(1, std::min(2, getTotalNumInputChannels()));
    scratch_.assign(static_cast<std::size_t>(std::max(1, samplesPerBlock)) * static_cast<std::size_t>(ch), 0.f);
    clock_ = &c->clock();
    ingress_ = &c->ingress();
    ss::app::AudioStreamFormat fmt;
    fmt.rateHz = static_cast<std::uint32_t>(sampleRate); fmt.channels = static_cast<std::uint32_t>(ch);
    fmt.bufferFrames = static_cast<std::uint32_t>(samplesPerBlock);
    ingress_->streamOpened(fmt);
}

void StageSignalProcessor::releaseResources() {
    if (ingress_) ingress_->streamLost();
}

bool StageSignalProcessor::isBusesLayoutSupported(const BusesLayout& l) const {
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo()) && in == out;
}

void StageSignalProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    // Real-time path (APP-SH-03): copy, interleave, enqueue. No allocation, lock, file or UI call.
    if (ingress_ != nullptr) {
        const int frames = buffer.getNumSamples();
        const int ch = std::min(2, getTotalNumInputChannels());
        if (ch > 0 && frames > 0 && scratch_.size() >= static_cast<std::size_t>(frames) * static_cast<std::size_t>(ch)) {
            for (int c = 0; c < ch; ++c) {
                const float* src = buffer.getReadPointer(c);
                float* dst = scratch_.data() + c;
                for (int i = 0; i < frames; ++i) dst[static_cast<std::size_t>(i) * static_cast<std::size_t>(ch)] = src[i];
            }
            ingress_->pushBlock(scratch_.data(), static_cast<std::uint32_t>(frames), static_cast<std::uint32_t>(ch),
                                static_cast<std::uint32_t>(rate_), clock_->nowMonotonic());
        }
    }
    // HostMode decides what the host hears: standalone is silent (monitoring off), CLAP is bit-exact passthrough.
    if (mode_->clearsProcessorOutput()) buffer.clear();
}

juce::AudioProcessorEditor* StageSignalProcessor::createEditor() { return new StageSignalEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new StageSignalProcessor(); }
