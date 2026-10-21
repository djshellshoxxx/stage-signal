#include "UI/DiagnosticsPanel.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include "UI/Overlays/OverlayText.h"
#include "UI/Theme.h"

namespace ss::app {

namespace {
constexpr std::size_t kMaxJournalRows = 200;
constexpr int kLeftW = 340;

const char* levelWord(DiagLevel l) {
    switch (l) {
        case DiagLevel::Info: return "Info";
        case DiagLevel::Warn: return "Warning";
        case DiagLevel::Error: return "Error";
    }
    return "Info";
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

// Journal rows: newest first, filtered by the search box (tag and text, case-insensitive).
class DiagnosticsPanel::JournalModel final : public juce::ListBoxModel {
public:
    explicit JournalModel(DiagnosticsPanel& owner) : owner_(owner) {}
    int getNumRows() override { return static_cast<int>(owner_.journalRows_.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
        if (row < 0 || row >= static_cast<int>(owner_.journalRows_.size())) return;
        const auto& e = owner_.journalRows_[static_cast<std::size_t>(row)];
        g.fillAll(selected ? theme::bgRaised() : theme::bgPanel());
        g.setFont(overlay::bodyFont(theme::kBodyFont));
        g.setColour(e.level == DiagLevel::Error ? theme::stateDanger()
                    : e.level == DiagLevel::Warn ? theme::stateWarn() : theme::textPrimary());
        const std::string left = std::string(levelWord(e.level)) + "  " + e.tag;
        g.drawText(juce::String(left), 6, 0, 170, h, juce::Justification::centredLeft, true);
        g.setColour(theme::textSecondary());
        g.drawText(juce::String(e.text), 176, 0, w - 232, h, juce::Justification::centredLeft, true);
        g.drawText(juce::String(overlay::agoText(owner_.nowNs_, e.atNs)), w - 58, 0, 54, h,
                   juce::Justification::centredRight, true);
    }
private:
    DiagnosticsPanel& owner_;
};

DiagnosticsPanel::DiagnosticsPanel(UiModel& model)
    : PanelBase(model, "Diagnostics"), doctor_(model), report_(model) {
    status_.setMultiLine(true);
    status_.setReadOnly(true);
    status_.setScrollbarsShown(true);
    status_.setFont(overlay::bodyFont(theme::kBodyFont));
    status_.setColour(juce::TextEditor::backgroundColourId, theme::bgSunken());
    status_.setColour(juce::TextEditor::textColourId, theme::textSecondary());
    status_.setColour(juce::TextEditor::outlineColourId, theme::lineDivider());

    journalModel_ = std::make_unique<JournalModel>(*this);
    journal_.setModel(journalModel_.get());
    journal_.setRowHeight(24);
    journal_.setColour(juce::ListBox::backgroundColourId, theme::bgPanel());
    journal_.setMultipleSelectionEnabled(false);

    filter_.setTextToShowWhenEmpty("Search the journal", theme::textMuted());
    filter_.setFont(overlay::bodyFont(theme::kBodyFont));
    filter_.setColour(juce::TextEditor::backgroundColourId, theme::bgSunken());
    filter_.setColour(juce::TextEditor::textColourId, theme::textPrimary());
    filter_.setColour(juce::TextEditor::outlineColourId, theme::lineControl());
    filter_.onTextChange = [this] {
        if (auto s = model_.snapshot()) refreshJournal(*s);
    };

    journalTitle_.setText("Event journal", juce::dontSendNotification);
    journalTitle_.setFont(overlay::bodyFont(theme::kBodyFont));
    journalTitle_.setColour(juce::Label::textColourId, theme::textPrimary());
    bundleStatus_.setFont(overlay::bodyFont(theme::kBodyFont));
    bundleStatus_.setColour(juce::Label::textColourId, theme::textSecondary());

    showReport_.onClick = [this] { report_.setVisible(!report_.isVisible()); resized(); };
    report_.onClosed = [this] { report_.setVisible(false); resized(); };
    report_.setVisible(false);
    supportBundle_.onClick = [this] {
        bundlePicker_.choose([this](const juce::File& f) {
            std::uint64_t corr = 0;
            AppCommand c;
            c.kind = AppCommandKind::ExportSupportBundle;
            c.text = f.getFullPathName().toStdString();
            if (overlay::sendCommand(model_, c, &corr)) {
                bundleCorrelation_ = corr;
                bundleStatus_.setText("Writing support bundle to " + f.getFileName() + ".", juce::dontSendNotification);
            } else {
                bundleStatus_.setText("Too many requests waiting. Try again in a moment.", juce::dontSendNotification);
            }
        });
    };

    addAndMakeVisible(doctor_);
    addAndMakeVisible(status_);
    addAndMakeVisible(filter_);
    addAndMakeVisible(journal_);
    addAndMakeVisible(journalTitle_);
    addAndMakeVisible(showReport_);
    addAndMakeVisible(supportBundle_);
    addAndMakeVisible(bundleStatus_);
    addChildComponent(report_);
}

DiagnosticsPanel::~DiagnosticsPanel() { journal_.setModel(nullptr); }

void DiagnosticsPanel::paint(juce::Graphics& g) { g.fillAll(theme::bgPanel()); }

void DiagnosticsPanel::resized() {
    auto r = getLocalBounds().reduced(8, 8);
    doctor_.setBounds(r.removeFromLeft(kLeftW));
    r.removeFromLeft(10);
    auto bottom = r.removeFromBottom(30);
    showReport_.setBounds(bottom.removeFromLeft(120).reduced(0, 2));
    bottom.removeFromLeft(8);
    supportBundle_.setBounds(bottom.removeFromLeft(170).reduced(0, 2));
    bundleStatus_.setBounds(bottom.reduced(8, 0));
    status_.setBounds(r.removeFromTop(150));
    r.removeFromTop(8);
    journalTitle_.setBounds(r.removeFromTop(20));
    filter_.setBounds(r.removeFromTop(26));
    journal_.setBounds(r);
    report_.setBounds(getLocalBounds().reduced(8, 8).withTrimmedLeft(kLeftW + 10).withTrimmedBottom(0));
}

void DiagnosticsPanel::uiSnapshotChanged(const UiSnapshot& s) {
    doctor_.observe(s);
    if (!doctor_.hasRun()) doctor_.runCheck(s);
    nowNs_ = s.atNs;
    refreshStatus(s);
    refreshJournal(s);
}

void DiagnosticsPanel::refreshStatus(const UiSnapshot& s) {
    std::string t;
    char buf[160];
    t += "Performance\n";
    std::snprintf(buf, sizeof buf, "  CPU %.0f %%, memory %.0f MB, render %.1f fps, frame %.1f ms (worst %.1f ms)\n",
                  s.perf.cpuPct, static_cast<double>(s.perf.workingSetBytes) / (1024.0 * 1024.0), s.perf.renderFps,
                  s.perf.renderMsLast, s.perf.renderMsWorst);
    t += s.perf.valid ? buf : "  Not measured yet\n";
    t += "Audio\n";
    std::snprintf(buf, sizeof buf, "  Device %s, signal %s, %u Hz, %u channels, %llu dropped blocks\n",
                  overlay::adapterWord(s.audio.deviceState), s.audio.signalAlive ? "alive" : "silent", s.audio.rateHz,
                  s.audio.channels, static_cast<unsigned long long>(s.audio.droppedBlocks));
    t += buf;
    if (!s.audio.deviceError.empty()) t += "  " + s.audio.deviceError + "\n";
    t += "Adapters\n";
    for (const auto& a : s.adapters) {
        t += "  " + a.name + ": " + overlay::adapterWord(a.state);
        if (!a.lastError.empty()) t += ", " + a.lastError;
        t += "\n";
    }
    t += "Threads\n";
    for (const auto& th : s.threads) {
        t += "  " + th.name + (th.running ? ", running" : ", stopped") + (th.stale ? ", no heartbeat" : "") + "\n";
    }
    std::snprintf(buf, sizeof buf, "Queues: %llu actions pending, %llu dropped, %llu diagnostic events dropped\n",
                  static_cast<unsigned long long>(s.actionsPending), static_cast<unsigned long long>(s.actionsDropped),
                  static_cast<unsigned long long>(s.diagDropped));
    t += buf;
    if (t != statusText_) {
        statusText_ = t;
        status_.setText(juce::String(t), false);
    }
}

void DiagnosticsPanel::refreshJournal(const UiSnapshot& s) {
    const auto needle = lower(filter_.getText().toStdString());
    // Rebuild only when the journal or the search text changed (the snapshot arrives at 30 Hz).
    const ss::Ns newest = s.recentDiag.empty() ? 0 : s.recentDiag.back().atNs;
    if (s.recentDiag.size() == journalKeyCount_ && newest == journalKeyNewest_ && needle == journalKeyNeedle_) return;
    journalKeyCount_ = s.recentDiag.size();
    journalKeyNewest_ = newest;
    journalKeyNeedle_ = needle;
    journalRows_.clear();
    for (auto it = s.recentDiag.rbegin(); it != s.recentDiag.rend() && journalRows_.size() < kMaxJournalRows; ++it) {
        if (!needle.empty() && lower(it->tag + " " + it->text).find(needle) == std::string::npos) continue;
        journalRows_.push_back(*it);
    }
    journal_.updateContent();
}

void DiagnosticsPanel::uiCommandOutcome(const CommandOutcome& o) {
    if (bundleCorrelation_ == 0 || o.correlationId != bundleCorrelation_ || o.kind != AppCommandKind::ExportSupportBundle) return;
    bundleCorrelation_ = 0;
    bundleStatus_.setText(juce::String(o.ok ? (o.message.empty() ? "Support bundle saved." : o.message)
                                            : (o.message.empty() ? "Support bundle did not finish." : o.message)),
                          juce::dontSendNotification);
}

}  // namespace ss::app
