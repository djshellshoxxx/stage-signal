// First-run setup wizard, 8 steps (app-ui 9, UJ-01). Keyboard path complete; nothing transmits during setup.
// Owner: ui-setup. The wizard is an inline panel inside Setup, not a modal dialog (app-ui 1, 9).
#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModel.h"

namespace ss::app::setup {

class Wizard final : public juce::Component {
public:
    static constexpr int kSteps = 8;

    struct Summary {
        std::vector<std::string> configured, skipped, off;
    };

    Wizard(UiModel& model, std::function<void()> onFinish, std::function<void()> onLeave);
    ~Wizard() override;

    int step() const { return step_; }          // 0-based; step 0 is "step 1 of 8"
    std::string stepTitle() const;
    Summary summary() const;

    // Feed every snapshot here; the audio step completes once a signal above -40 dBFS has been seen.
    void observe(const UiSnapshot& s);

    // Test and keyboard entry points (the same code paths as the buttons).
    void next();
    void back();
    void skip();
    void finish();
    bool canAdvance() const;

    void resized() override;
    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    void showStep();
    bool saveProgress(bool completed);
    void askLeave();
    void recordAndAdvance(bool wasSkipped);

    UiModel& model_;
    std::function<void()> onFinish_, onLeave_;
    int step_ = 0;
    bool benchmarkRunning_ = false;
    bool audioSeen_ = false, audioSkipped_ = false, leavePrompt_ = false;
    float lastPeakDb_ = -120.f;
    std::array<unsigned char, kSteps> result_{};   // 0 open, 1 configured, 2 skipped

    juce::Label header_, body_, detail_, leaveText_;
    juce::TextButton back_, skip_, next_, cancel_, action_, keep_, discard_;
    std::unique_ptr<juce::FileChooser> chooser_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Wizard)
};

}  // namespace ss::app::setup
