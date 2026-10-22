#include "UI/Overlays/ShowReportView.h"
#include "UI/Overlays/OverlayText.h"
#include "UI/Theme.h"
#include <utility>

namespace ss::app {

ShowReportView::ShowReportView(UiModel& model) : model_(model) {
    title_.setText("Show Report", juce::dontSendNotification);
    title_.setFont(overlay::bodyFont(theme::kBodyFont + 2.0f));
    title_.setColour(juce::Label::textColourId, theme::textPrimary());

    rangeLabel_.setText("Range", juce::dontSendNotification);
    range_.addItem("This session", 1);
    range_.addItem("Since last start", 2);
    range_.addItem("Custom range", 3);
    range_.setSelectedId(1, juce::dontSendNotification);
    range_.onChange = [this] {
        const bool custom = range_.getSelectedId() == 3;
        from_.setVisible(custom);
        to_.setVisible(custom);
        fromLabel_.setVisible(custom);
        toLabel_.setVisible(custom);
        resized();
    };
    fromLabel_.setText("From (UTC)", juce::dontSendNotification);
    toLabel_.setText("To (UTC)", juce::dontSendNotification);
    for (auto* e : {&from_, &to_}) {
        e->setTextToShowWhenEmpty("2026-10-21 19:00", theme::textMuted());
        e->setColour(juce::TextEditor::backgroundColourId, theme::bgSunken());
        e->setColour(juce::TextEditor::textColourId, theme::textPrimary());
        e->setColour(juce::TextEditor::outlineColourId, theme::lineControl());
        e->setInputRestrictions(24, "0123456789-: TZ");
        e->setVisible(false);
    }
    fromLabel_.setVisible(false);
    toLabel_.setVisible(false);

    for (auto* t : {&setlist_, &timeline_, &markers_, &devices_, &recording_}) {
        t->setToggleState(true, juce::dontSendNotification);
        t->setColour(juce::ToggleButton::textColourId, theme::textPrimary());
    }

    formatLabel_.setText("Format", juce::dontSendNotification);
    format_.addItem("HTML", 1);
    format_.addItem("CSV", 2);
    format_.addItem("HTML and CSV", 3);
    format_.setSelectedId(1, juce::dontSendNotification);

    folderLabel_.setText("No folder chosen", juce::dontSendNotification);
    chooseFolder_.onClick = [this] {
        folderPicker_.choose([this](const juce::File& f) {
            folderPath_ = f.getFullPathName().toStdString();
            folderLabel_.setText(f.getFileName(), juce::dontSendNotification);
        });
    };

    previewText_.setText(privacyStatement(), juce::dontSendNotification);
    previewText_.setJustificationType(juce::Justification::topLeft);
    preview_.onClick = [this] {
        previewText_.setText(juce::String(includedNames()) + "\n" + privacyStatement(), juce::dontSendNotification);
    };
    export_.onClick = [this] { status_.setText(juce::String(exportReport()), juce::dontSendNotification); };
    cancel_.onClick = [this] { if (onClosed) onClosed(); };

    for (auto* l : {&rangeLabel_, &fromLabel_, &toLabel_, &formatLabel_, &folderLabel_, &previewText_, &status_}) {
        l->setFont(overlay::bodyFont(theme::kBodyFont));
        l->setColour(juce::Label::textColourId, theme::textSecondary());
    }
    previewText_.setColour(juce::Label::textColourId, theme::textPrimary());

    for (auto* c : std::initializer_list<juce::Component*>{&title_, &rangeLabel_, &range_, &fromLabel_, &from_, &toLabel_,
                                                           &to_, &setlist_, &timeline_, &markers_, &devices_, &recording_,
                                                           &formatLabel_, &format_, &chooseFolder_, &folderLabel_,
                                                           &preview_, &previewText_, &export_, &cancel_, &status_})
        addAndMakeVisible(c);
    model_.addListener(this);
}

ShowReportView::~ShowReportView() { model_.removeListener(this); }

std::string ShowReportView::includedNames() const {
    std::string out = "Includes: ";
    const std::pair<const juce::ToggleButton*, const char*> items[] = {
        {&setlist_, "setlist"}, {&timeline_, "energy, BPM and key timeline"}, {&markers_, "markers"},
        {&devices_, "device events"}, {&recording_, "recording and broadcast changes"}};
    bool first = true;
    for (const auto& [button, name] : items) {
        if (!button->getToggleState()) continue;
        out += (first ? "" : ", ");
        out += name;
        first = false;
    }
    return first ? "Includes: nothing. Pick at least one item." : out + ".";
}

const char* ShowReportView::privacyStatement() {
    return "No secrets, no file paths, no audio or video.";
}

std::uint32_t ShowReportView::includeMask() const {
    std::uint32_t m = 0;
    if (setlist_.getToggleState()) m |= 1u;
    if (timeline_.getToggleState()) m |= 2u;
    if (markers_.getToggleState()) m |= 4u;
    if (devices_.getToggleState()) m |= 8u;
    if (recording_.getToggleState()) m |= 16u;
    return m;
}

int ShowReportView::formatCode() const { return format_.getSelectedId() == 0 ? 1 : format_.getSelectedId(); }
int ShowReportView::rangeCode() const { return range_.getSelectedId() == 0 ? 1 : range_.getSelectedId(); }

std::string ShowReportView::exportReport() {
    if (folderPath_.empty()) return "Choose a folder first.";
    if (includeMask() == 0) return "Pick at least one thing to include.";
    AppCommand c;
    c.kind = AppCommandKind::ExportShowReport;
    c.text = folderPath_;
    c.a = static_cast<std::int64_t>(includeMask());
    c.b = formatCode();
    c.value = static_cast<double>(rangeCode());
    if (rangeCode() == 3) {
        const auto f = from_.getText().toStdString(), t = to_.getText().toStdString();
        if (f.empty() || t.empty()) return "Enter a start and end time for a custom range.";
        c.text2 = f + "|" + t;
    }
    std::uint64_t corr = 0;
    if (!overlay::sendCommand(model_, c, &corr)) return "Too many requests waiting. Try again in a moment.";
    pendingCorrelation_ = corr;
    return "Exporting. The result appears here.";
}

void ShowReportView::uiCommandOutcome(const CommandOutcome& o) {
    if (pendingCorrelation_ == 0 || o.correlationId != pendingCorrelation_ || o.kind != AppCommandKind::ExportShowReport) return;
    pendingCorrelation_ = 0;
    if (o.ok) status_.setText(o.message.empty() ? "Report saved." : juce::String(o.message), juce::dontSendNotification);
    else status_.setText(o.message.empty() ? "Export did not finish. Try another folder." : juce::String(o.message),
                         juce::dontSendNotification);
}

void ShowReportView::paint(juce::Graphics& g) {
    g.fillAll(theme::bgPanel());
    g.setColour(theme::lineDivider());
    g.drawRect(getLocalBounds(), 1);
}

void ShowReportView::resized() {
    auto r = getLocalBounds().reduced(10, 8);
    title_.setBounds(r.removeFromTop(24));
    auto row = [&r](int h) { return r.removeFromTop(h); };
    auto a = row(26);
    rangeLabel_.setBounds(a.removeFromLeft(60));
    range_.setBounds(a.removeFromLeft(170).reduced(0, 2));
    if (range_.getSelectedId() == 3) {
        a.removeFromLeft(8);
        fromLabel_.setBounds(a.removeFromLeft(70));
        from_.setBounds(a.removeFromLeft(130).reduced(0, 2));
        a.removeFromLeft(8);
        toLabel_.setBounds(a.removeFromLeft(56));
        to_.setBounds(a.removeFromLeft(130).reduced(0, 2));
    }
    for (auto* t : {&setlist_, &timeline_, &markers_, &devices_, &recording_}) t->setBounds(row(22));
    auto f = row(28);
    formatLabel_.setBounds(f.removeFromLeft(60));
    format_.setBounds(f.removeFromLeft(170).reduced(0, 2));
    auto folder = row(28);
    chooseFolder_.setBounds(folder.removeFromLeft(120).reduced(0, 2));
    folderLabel_.setBounds(folder.reduced(8, 0));
    auto buttons = row(30);
    preview_.setBounds(buttons.removeFromLeft(190).reduced(0, 2));
    buttons.removeFromLeft(8);
    export_.setBounds(buttons.removeFromLeft(90).reduced(0, 2));
    buttons.removeFromLeft(8);
    cancel_.setBounds(buttons.removeFromLeft(80).reduced(0, 2));
    status_.setBounds(row(22));
    previewText_.setBounds(r.removeFromBottom(44));
}

}  // namespace ss::app
