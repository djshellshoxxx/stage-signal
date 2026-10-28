#include "UI/Setup/Wizard.h"
#include "UI/Live/Actions.h"
#include "UI/Theme.h"
#include "UI/Setup/WizardProgress.h"
#include <algorithm>

namespace ss::app::setup {

using uikit::sendAction;

namespace {
const char* kTitles[Wizard::kSteps] = {"Welcome", "Displays", "Audio", "Camera (optional)", "MIDI (optional)",
                                       "Lighting interface (optional)", "Recording folder", "Benchmark and summary"};
}

Wizard::Wizard(UiModel& model, std::function<void()> onFinish, std::function<void()> onLeave)
    : model_(model), onFinish_(std::move(onFinish)), onLeave_(std::move(onLeave)) {
    for (auto* l : {&header_, &body_, &detail_, &leaveText_}) {
        addAndMakeVisible(*l);
        l->setFont(juce::FontOptions(theme::kBodyFont));
        l->setColour(juce::Label::textColourId, theme::textPrimary());
        l->setJustificationType(juce::Justification::topLeft);
    }
    header_.setFont(juce::FontOptions(16.0f));
    detail_.setColour(juce::Label::textColourId, theme::textSecondary());
    leaveText_.setText("Leave setup? Your changes so far are kept.", juce::dontSendNotification);
    auto add = [this](juce::Component& c, const char* id, const char* text) {
        c.setComponentID(id);
        addAndMakeVisible(c);
        if (auto* b = dynamic_cast<juce::Button*>(&c)) b->setButtonText(text);
    };
    add(back_, "wiz.back", "Back");
    add(skip_, "wiz.skip", "Skip this step");
    add(next_, "wiz.next", "Next");
    add(cancel_, "wiz.cancel", "Cancel");
    add(action_, "wiz.action", "");
    add(keep_, "wiz.keep", "Keep");
    add(discard_, "wiz.discard", "Discard");

    back_.onClick = [this] { back(); };
    skip_.onClick = [this] { skip(); };
    next_.onClick = [this] { next(); };
    cancel_.onClick = [this] { askLeave(); };
    keep_.onClick = [this] { leavePrompt_ = false; showStep(); };
    discard_.onClick = [this] { if (onLeave_) onLeave_(); };
    action_.onClick = [this] {
        if (step_ == 1) {
            sendAction(model_, ss::ActionId::IdentifyDisplays, 0.0);
            detail_.setText("Numbers are shown on each display for 5 seconds.", juce::dontSendNotification);
        } else if (step_ == 7) {
            AppCommand command;
            command.kind = benchmarkRunning_ ? AppCommandKind::CancelBenchmark : AppCommandKind::RunBenchmark;
            model_.postCommand(command);
        } else if (step_ == 6) {
            chooser_ = std::make_unique<juce::FileChooser>("Choose the recording folder", juce::File(), "", true);
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [this](const juce::FileChooser& fc) {
                                      const auto dir = fc.getResult();
                                      if (dir == juce::File()) return;
                                      ss::app::AppCommand c;
                                      c.kind = AppCommandKind::SetRecordFolder;
                                      c.text = dir.getFullPathName().toStdString();
                                      model_.postCommand(c);
                                      detail_.setText("Recording folder chosen. The write test runs when recording is set up.", juce::dontSendNotification);
                                  });
        }
    };
    if (auto snapshot = model_.snapshot()) for (const auto& pref : snapshot->prefs) {
        WizardProgress progress;
        if (pref.first != "wizard.progress" || !WizardProgress::decode(pref.second, progress) || progress.completed) continue;
        step_ = progress.step; audioSeen_ = progress.audioSeen; audioSkipped_ = progress.audioSkipped; result_ = progress.results;
    }
    showStep();
}

Wizard::~Wizard() = default;

std::string Wizard::stepTitle() const { return kTitles[step_]; }

bool Wizard::canAdvance() const {
    if (step_ == 2) return audioSeen_ || audioSkipped_;
    if (step_ == 7) return !benchmarkRunning_;
    return true;
}

void Wizard::observe(const UiSnapshot& s) {
    benchmarkRunning_ = s.benchmark.running;
    if (step_ == 7) {
        action_.setButtonText(benchmarkRunning_ ? "Cancel benchmark" : "Run quick benchmark");
        action_.setTooltip(juce::String(s.benchmark.note));
        next_.setEnabled(canAdvance());
    }
    lastPeakDb_ = std::max(s.features.peakDb[0], s.features.peakDb[1]);
    if (step_ == 2 && lastPeakDb_ > -40.f) audioSeen_ = true;
    if (step_ == 2) {
        detail_.setText("Level: " + juce::String(static_cast<int>(lastPeakDb_)) + " dB peak. "
                        + (audioSeen_ ? "Signal seen. You can go on." : "Waiting for a signal above minus 40 dB."),
                        juce::dontSendNotification);
        next_.setEnabled(canAdvance());
    }
}

void Wizard::next() {
    if (!canAdvance()) return;
    recordAndAdvance(false);
}

void Wizard::skip() {
    if (step_ == 7 && benchmarkRunning_) {
        AppCommand command; command.kind = AppCommandKind::CancelBenchmark; model_.postCommand(command);
        benchmarkRunning_ = false;
    }
    if (step_ == 2) audioSkipped_ = true;
    recordAndAdvance(true);
}

void Wizard::back() {
    if (step_ == 0) return;
    --step_;
    saveProgress(false);
    showStep();
}

// Built from the per-step results, so going back and redoing a step never double counts.
Wizard::Summary Wizard::summary() const {
    Summary out;
    for (int i = 0; i < kSteps; ++i) {
        if (result_[static_cast<std::size_t>(i)] == 1) out.configured.push_back(kTitles[i]);
        else if (result_[static_cast<std::size_t>(i)] == 2) out.skipped.push_back(kTitles[i]);
    }
    if (result_[2] == 2) out.skipped.push_back("Audio (not verified: no signal was seen)");
    out.off.push_back("Lighting: Disarmed. Arm it on the MIDI/Lighting tab.");
    out.off.push_back("MIDI output: off");
    out.off.push_back("Broadcast: off");
    out.off.push_back("Recording: off until you press Record");
    return out;
}

void Wizard::finish() {
    if (step_ != kSteps - 1 || !saveProgress(true)) return;
    if (onFinish_) onFinish_();
}

bool Wizard::saveProgress(bool completed) {
    WizardProgress progress;
    progress.step = step_; progress.completed = completed; progress.audioSeen = audioSeen_; progress.audioSkipped = audioSkipped_;
    progress.results = result_;
    AppCommand command; command.kind = AppCommandKind::SetPref; command.text = "wizard.progress"; command.text2 = progress.encode();
    if (model_.postCommand(command)) return true;
    detail_.setText("Setup progress could not be queued. Try again in a moment.", juce::dontSendNotification);
    return false;
}

void Wizard::recordAndAdvance(bool wasSkipped) {
    result_[static_cast<std::size_t>(step_)] = wasSkipped ? 2 : 1;
    if (step_ < kSteps - 1) {
        ++step_;
        saveProgress(false);
        showStep();
    } else {
        finish();
    }
}

void Wizard::askLeave() {
    leavePrompt_ = true;
    showStep();
}

void Wizard::showStep() {
    const bool prompt = leavePrompt_;
    header_.setText("Setup, step " + juce::String(step_ + 1) + " of " + juce::String(kSteps) + ": " + kTitles[step_],
                    juce::dontSendNotification);
    const char* text = "";
    juce::String actionText;
    switch (step_) {
        case 0: text = "This wizard sets up displays, audio, and the optional inputs and outputs. Nothing transmits during setup. DMX and broadcast stay off."; break;
        case 1: text = "Assign a Controls display and, if you use one, an Audience display. A single display is fine for practice."; actionText = "Identify displays"; break;
        case 2: text = "Play a loud part of your set. Turn the gain down on your interface until the CLIP light stays off. Aim for peaks around minus 12 to minus 6."; break;
        case 3: text = "Camera choice is not available in this build. Skip this step, or set the camera later in Media."; break;
        case 4: text = "MIDI mapping is done on the MIDI/Lighting tab. Skip this step for now."; break;
        case 5: text = "Lighting protocol and destination are set on the MIDI/Lighting tab. You arm it later there. Lighting stays Disarmed."; break;
        case 6: text = "Pick the folder where recordings are saved. Recording stays off until you press Record."; actionText = "Choose folder"; break;
        case 7: actionText = "Run quick benchmark"; text = "The software benchmark uses discard outputs only. Missing device and latency evidence keeps the profile unqualified. The summary lists what is set up and what is off."; break;
        default: break;
    }
    body_.setText(text, juce::dontSendNotification);
    juce::String detailText = step_ == 2 ? "Level: waiting for signal." : "";
    if (step_ == kSteps - 1) {
        const auto sum = summary();
        auto join = [](const std::vector<std::string>& v) {
            juce::String out;
            for (const auto& x : v) out << juce::String(x) << "; ";
            return out.isEmpty() ? juce::String("none") : out;
        };
        detailText = "Set up: " + join(sum.configured) + "\nSkipped: " + join(sum.skipped) + "\nOff: " + join(sum.off);
    }
    detail_.setText(detailText, juce::dontSendNotification);
    action_.setButtonText(actionText);
    action_.setVisible(actionText.isNotEmpty());
    back_.setEnabled(step_ > 0);
    skip_.setVisible(step_ > 0);
    next_.setButtonText(step_ == kSteps - 1 ? "Finish" : "Next");
    next_.setEnabled(canAdvance());
    const bool show = !prompt;
    for (auto* c : std::initializer_list<juce::Component*>{&back_, &skip_, &next_, &cancel_, &action_, &header_, &body_, &detail_})
        c->setVisible(show);
    leaveText_.setVisible(prompt);
    keep_.setVisible(prompt);
    discard_.setVisible(prompt);
    if (prompt) keep_.grabKeyboardFocus();
    else next_.grabKeyboardFocus();
    resized();
    repaint();
}

void Wizard::resized() {
    auto r = getLocalBounds().reduced(16);
    header_.setBounds(r.removeFromTop(28));
    auto footer = r.removeFromBottom(40);
    body_.setBounds(r.removeFromTop(120));
    detail_.setBounds(r.removeFromTop(60));
    action_.setBounds(r.removeFromTop(40).removeFromLeft(220));
    leaveText_.setBounds(r.removeFromTop(40));
    // Footer row: Back, Skip, Next, Cancel on the right. Leave prompt: Keep, Discard.
    uikit::placeRow(footer, {{&back_, 90}, {&skip_, 140}, {&keep_, 0}, {&discard_, 110}, {&cancel_, 100}, {&next_, 110}});
}

void Wizard::paint(juce::Graphics& g) {
    g.fillAll(theme::bgPanel());
    // Progress dots: text as well as shape, so the step is clear without colour (APP-UI-03).
    juce::String dots;
    for (int i = 0; i < kSteps; ++i) dots << (i == step_ ? "[" : " ") << (i < step_ ? "*" : (i == step_ ? "#" : ".")) << (i == step_ ? "]" : " ");
    g.setColour(theme::textSecondary());
    g.setFont(juce::FontOptions(theme::kBodyFont));
    g.drawText(dots, getLocalBounds().removeFromBottom(60).removeFromTop(20), juce::Justification::centredLeft, false);
}

bool Wizard::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::escapeKey) {
        if (leavePrompt_) { leavePrompt_ = false; showStep(); } else askLeave();
        return true;
    }
    return false;
}

}  // namespace ss::app::setup
