#include "UI/Overlays/ShowDoctorView.h"
#include "UI/Overlays/OverlayText.h"
#include "UI/Theme.h"

namespace ss::app {

namespace {
constexpr int kRowH = 66, kBannerH = 26, kStatusH = 22;

const char* severityWord(doctor::Severity s) {
    switch (s) {
        case doctor::Severity::Blocker: return "Blocker";
        case doctor::Severity::Warning: return "Warning";
        case doctor::Severity::Info: return "Info";
    }
    return "Info";
}

juce::Colour severityColour(doctor::Severity s) {
    switch (s) {
        case doctor::Severity::Blocker: return theme::stateDanger();
        case doctor::Severity::Warning: return theme::stateWarn();
        case doctor::Severity::Info: return theme::textSecondary();
    }
    return theme::textSecondary();
}
}  // namespace

// One finding: severity word (text, not colour alone), message, cause on request, and one button.
class ShowDoctorView::Row final : public juce::Component {
public:
    Row(ShowDoctorView& owner, std::size_t index, const doctor::Finding& f) : owner_(owner), index_(index) {
        sev_.setText(severityWord(f.severity), juce::dontSendNotification);
        sev_.setColour(juce::Label::textColourId, severityColour(f.severity));
        sev_.setFont(overlay::bodyFont(theme::kBodyFont));
        msg_.setText(juce::String(f.message), juce::dontSendNotification);
        msg_.setColour(juce::Label::textColourId, theme::textPrimary());
        msg_.setFont(overlay::bodyFont(theme::kBodyFont));
        why_.setText(juce::String(f.why), juce::dontSendNotification);
        why_.setColour(juce::Label::textColourId, theme::textSecondary());
        why_.setFont(overlay::bodyFont(theme::kBodyFont));
        why_.setVisible(false);
        const bool canFix = f.fix.has_value() && doctor::Doctor::isSafeFixAction(f.fix->action);
        if (canFix) {
            button_.setButtonText(juce::String(f.fix->label));
            button_.onClick = [this] { owner_.applyFix(index_); };
        } else {
            button_.setButtonText("Details");
            button_.onClick = [this] { why_.setVisible(!why_.isVisible()); button_.setButtonText(why_.isVisible() ? "Hide" : "Details"); resized(); };
        }
        addAndMakeVisible(sev_); addAndMakeVisible(msg_); addAndMakeVisible(why_); addAndMakeVisible(button_);
    }

    void paint(juce::Graphics& g) override {
        g.setColour(theme::lineDivider());
        g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
    }

    void resized() override {
        auto r = getLocalBounds().reduced(4, 4);
        button_.setBounds(r.removeFromRight(96).withSizeKeepingCentre(96, 28));
        r.removeFromRight(8);
        auto top = r.removeFromTop(20);
        sev_.setBounds(top.removeFromLeft(72));
        msg_.setBounds(top);
        if (why_.isVisible()) why_.setBounds(r.removeFromTop(20));
    }

private:
    ShowDoctorView& owner_;
    std::size_t index_;
    juce::Label sev_, msg_, why_;
    juce::TextButton button_;
};

ShowDoctorView::ShowDoctorView(UiModel& model) : model_(model) {
    banner_.setFont(overlay::bodyFont(theme::kBodyFont + 2.0f));
    banner_.setColour(juce::Label::textColourId, theme::textPrimary());
    banner_.setText("Not checked yet. Run a check to see what is ready.", juce::dontSendNotification);
    status_.setFont(overlay::bodyFont(theme::kBodyFont));
    status_.setColour(juce::Label::textColourId, theme::textSecondary());
    runButton_.onClick = [this] { if (auto s = model_.snapshot()) runCheck(*s); };
    viewport_.setViewedComponent(&body_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(banner_); addAndMakeVisible(runButton_); addAndMakeVisible(viewport_); addAndMakeVisible(status_);
}

ShowDoctorView::~ShowDoctorView() { viewport_.setViewedComponent(nullptr, false); }

void ShowDoctorView::paint(juce::Graphics& g) {
    g.fillAll(theme::bgRaised());
}

void ShowDoctorView::resized() {
    auto r = getLocalBounds().reduced(8, 8);
    auto top = r.removeFromTop(kBannerH);
    runButton_.setBounds(top.removeFromRight(110));
    banner_.setBounds(top);
    status_.setBounds(r.removeFromBottom(kStatusH));
    viewport_.setBounds(r);
    layoutRows();
}

void ShowDoctorView::layoutRows() {
    const int w = viewport_.getMaximumVisibleWidth();
    int y = 0;
    for (auto& row : rows_) {
        row->setBounds(0, y, w, kRowH);
        y += kRowH;
    }
    body_.setSize(w, juce::jmax(y, viewport_.getHeight()));
}

void ShowDoctorView::observe(const UiSnapshot& s) {
    if (s.audio.signalAlive) silentSinceNs_ = -1;
    else if (silentSinceNs_ < 0) silentSinceNs_ = s.atNs;
}

doctor::EnvironmentSnapshot ShowDoctorView::environmentFrom(const UiSnapshot& s) const {
    doctor::EnvironmentSnapshot e;
    const auto& a = s.audio;
    e.audio.deviceSelected = a.deviceState != AdapterState::Unavailable;
    e.audio.deviceOpen = a.deviceState == AdapterState::Running || a.deviceState == AdapterState::Degraded;
    e.audio.deviceLost = a.deviceState == AdapterState::Fault;
    if (!a.signalAlive && silentSinceNs_ >= 0 && s.atNs > silentSinceNs_)
        e.audio.silentSeconds = static_cast<double>(s.atNs - silentSinceNs_) / static_cast<double>(ss::kNsPerSec);
    e.audio.clipping = s.features.inputNearFullScale;
    e.audio.clipRunLength = s.features.clipRunLength;
    e.disk.known = false;                       // free space is not in the UI model yet: reported as unknown
    e.lighting.configured = s.lighting.transportConnected;
    e.lighting.armed = s.lighting.state == LightingUiState::Armed;
    e.lighting.dryRun = s.lighting.state == LightingUiState::DryRun;
    e.lighting.destinationSet = s.lighting.transportConnected;
    e.safety.strobingDisabled = s.show.strobingDisabled;
    e.safety.visualBlackoutLatched = s.show.visualBlackout;
    e.safety.lightingBlackoutLatched = s.show.lightingBlackout;
    e.benchmark.run = false;                    // benchmark results are not in the UI model yet
    e.networkDisabled = false;
    return e;
}

std::string ShowDoctorView::bannerFor(const std::vector<doctor::Finding>& f) {
    int blockers = 0, warnings = 0;
    for (const auto& x : f) {
        if (x.severity == doctor::Severity::Blocker) ++blockers;
        else if (x.severity == doctor::Severity::Warning) ++warnings;
    }
    if (blockers > 0) return "Not ready: " + std::to_string(blockers) + (blockers == 1 ? " blocker" : " blockers");
    if (warnings > 0) return "Ready with " + std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings");
    return "Ready";
}

void ShowDoctorView::runCheck(const UiSnapshot& s) {
    findings_ = doctor::Doctor::run(environmentFrom(s));
    hasRun_ = true;
    banner_.setText(juce::String(bannerFor(findings_)), juce::dontSendNotification);
    status_.setText("", juce::dontSendNotification);
    rebuildRows();
}

void ShowDoctorView::rebuildRows() {
    body_.removeAllChildren();
    rows_.clear();
    for (std::size_t i = 0; i < findings_.size(); ++i) {
        rows_.push_back(std::make_unique<Row>(*this, i, findings_[i]));
        body_.addAndMakeVisible(*rows_.back());
    }
    layoutRows();
}

void ShowDoctorView::applyFix(std::size_t index) {
    if (index >= findings_.size() || !findings_[index].fix) return;
    const auto& fix = *findings_[index].fix;
    status_.setText(juce::String(overlay::sendAction(model_, fix.action, fix.value, kAllOutputs, nullptr)),
                    juce::dontSendNotification);
}

}  // namespace ss::app
