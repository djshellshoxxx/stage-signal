#include "UI/SetupPanel.h"
#include "UI/Live/Actions.h"
#include "UI/Live/Shortcuts.h"
#include "UI/Setup/AppearanceSettings.h"
#include <functional>

namespace ss::app {

using uikit::sendAction;

namespace {
constexpr int kMaxOutputRows = 16;   // ss::show::Limits::kMaxOutputs
constexpr int kRowH = 32, kGap = 6, kHeaderH = 26;
}

// Setup content. Sections stack vertically in one scrolling column (app-ui 5.2). Sections do not collapse yet.
class SetupPanel::Content final : public juce::Component {
public:
    Content(UiModel& m, audience::OutputManager& outputs, juce::Component& operatorTop)
        : model_(m), outputs_(outputs), top_(operatorTop) {
        setComponentID("setup.content");
        auto add = [this](juce::Component& c, const char* id) { c.setComponentID(id); addAndMakeVisible(c); };
        auto header = [this](juce::Label& l, const char* text) {
            l.setText(text, juce::dontSendNotification);
            l.setFont(juce::FontOptions(16.0f));
            l.setColour(juce::Label::textColourId, theme::textPrimary());
            addAndMakeVisible(l);
        };
        auto note = [this](juce::Label& l) {
            l.setFont(juce::FontOptions(theme::kBodyFont));
            l.setColour(juce::Label::textColourId, theme::textSecondary());
            l.setMinimumHorizontalScale(1.0f);
            addAndMakeVisible(l);
        };

        add(runWizard_, "setup.wizard");
        runWizard_.setButtonText("Run setup wizard");
        runWizard_.setTooltip("Walks through displays, audio, inputs and outputs. Nothing transmits.");
        runWizard_.onClick = [this] { if (onWizard) onWizard(); };

        header(audioHead_, "Audio");
        note(audioState_);
        add(deviceType_, "setup.audio.type");
        for (const char* t : {"Default", "WASAPI", "DirectSound", "CoreAudio", "ALSA", "JACK", "ASIO"})
            deviceType_.addItem(t, deviceType_.getNumItems() + 1);
        deviceType_.setSelectedId(1, juce::dontSendNotification);
        deviceType_.setTooltip("Audio driver type");
        add(deviceName_, "setup.audio.name");
        deviceName_.setTextToShowWhenEmpty("Device name, as shown by the system", theme::textMuted());
        deviceName_.setTooltip("Type the device name exactly as the system shows it");
        add(applyDevice_, "setup.audio.apply");
        applyDevice_.setButtonText("Use this device");
        applyDevice_.onClick = [this] {
            AppCommand c;
            c.kind = AppCommandKind::SelectAudioDevice;
            c.text = deviceType_.getText().toStdString();
            c.text2 = deviceName_.getText().toStdString();
            if (!model_.postCommand(c)) notice_.setText("Too many requests. Try again in a moment.", juce::dontSendNotification);
            else notice_.setText("Switching device. This can take a moment.", juce::dontSendNotification);
        };
        add(rate_, "setup.audio.rate");
        rate_.addItem("44.1 kHz", 1);
        rate_.addItem("48 kHz", 2);
        rate_.setSelectedId(2, juce::dontSendNotification);
        rate_.onChange = [this] {
            AppCommand c;
            c.kind = AppCommandKind::SetAudioRate;
            c.a = rate_.getSelectedId() == 1 ? 44100 : 48000;
            model_.postCommand(c);
        };
        note(rateNote_);
        rateNote_.setText("Only 44.1 kHz and 48 kHz are supported.", juce::dontSendNotification);

        header(displayHead_, "Displays and audience outputs");
        note(displayNote_);
        add(displayBox_, "setup.display.box");
        add(refreshDisplays_, "setup.display.refresh");
        refreshDisplays_.setButtonText("Refresh displays");
        refreshDisplays_.onClick = [this] { refreshDisplayList(); };
        add(identify_, "setup.display.identify");
        identify_.setButtonText("Identify displays");
        identify_.setTooltip("Shows a big number on each display for 5 seconds");
        identify_.onClick = [this] { sendAction(model_, ss::ActionId::IdentifyDisplays); };
        refreshDisplayList();

        for (int i = 0; i < kMaxOutputRows; ++i) {
            auto& r = rows_[static_cast<std::size_t>(i)];
            r.name = std::make_unique<juce::Label>();
            r.open = std::make_unique<juce::TextButton>();
            r.close = std::make_unique<juce::TextButton>();
            r.overlay = std::make_unique<juce::ToggleButton>();
            note(*r.name);
            add(*r.open, ("setup.out." + std::to_string(i) + ".open").c_str());
            add(*r.close, ("setup.out." + std::to_string(i) + ".close").c_str());
            add(*r.overlay, ("setup.out." + std::to_string(i) + ".overlay").c_str());
            r.open->setButtonText("Open on display");
            r.close->setButtonText("Close");
            r.overlay->setButtonText("BPM and key overlay");
            r.open->onClick = [this, i] { openRow(i); };
            r.close->onClick = [this, i] { closeRow(i); };
            r.overlay->onClick = [this, i] {
                auto& row = rows_[static_cast<std::size_t>(i)];
                sendAction(model_, ss::ActionId::ToggleOverlayBpmKey, row.overlay->getToggleState() ? 1.0 : 0.0, row.id);
            };
        }

        header(safetyHead_, "Safety");
        add(strobe_, "setup.safety.strobe");
        strobe_.setButtonText("Disable strobing");
        strobe_.setTooltip("Locks off flashing scenes and strobe channels");
        strobe_.onClick = [this] {
            sendAction(model_, strobe_.getToggleState() ? ss::ActionId::DisableStrobing : ss::ActionId::EnableStrobing);
        };
        note(strobeNote_);

        header(appearanceHead_, "Appearance");
        add(theme_, "setup.theme");
        theme_.addItem("Night", 1);
        theme_.addItem("High contrast", 2);
        theme_.setSelectedId(setup::appearance().theme == setup::ThemeChoice::HighContrast ? 2 : 1, juce::dontSendNotification);
        theme_.onChange = [this] {
            setup::appearance().theme = theme_.getSelectedId() == 2 ? setup::ThemeChoice::HighContrast : setup::ThemeChoice::Night;
        };
        add(clean_, "setup.clean");
        clean_.setButtonText("Clean mode");
        clean_.setToggleState(setup::appearance().cleanMode, juce::dontSendNotification);
        clean_.onClick = [this] { setup::appearance().cleanMode = clean_.getToggleState(); };
        add(reduce_, "setup.reduce");
        reduce_.setButtonText("Reduce motion");
        reduce_.setToggleState(setup::appearance().reduceMotion, juce::dontSendNotification);
        reduce_.onClick = [this] { setup::appearance().reduceMotion = reduce_.getToggleState(); };
        note(appearanceNote_);
        appearanceNote_.setText("Appearance choices last for this session only.", juce::dontSendNotification);

        header(filesHead_, "Show files");
        add(save_, "setup.file.save");
        save_.setButtonText("Save (Ctrl+S)");
        save_.onClick = [this] { sendAction(model_, ss::ActionId::SaveShow); };
        add(saveAs_, "setup.file.saveas");
        saveAs_.setButtonText("Save a copy...");
        saveAs_.onClick = [this] { chooseFile(true); };
        add(open_, "setup.file.open");
        open_.setButtonText("Open show...");
        open_.onClick = [this] { chooseFile(false); };

        header(benchHead_, "Benchmark and privacy");
        note(benchNote_);
        benchNote_.setText("Benchmark is not available in this build. Privacy opt-ins come later. Both are off.", juce::dontSendNotification);

        add(notice_, "setup.notice");
        notice_.setFont(juce::FontOptions(theme::kBodyFont));
        notice_.setColour(juce::Label::textColourId, theme::stateWarn());
    }

    std::function<void()> onWizard;
    juce::Component* control(const juce::String& id) { return uikit::findById(this, id); }
    juce::String notice() const { return notice_.getText(); }
    void setNotice(const juce::String& s) { notice_.setText(s, juce::dontSendNotification); }

    void apply(const UiSnapshot& s) {
        audioState_.setText(audioText(s), juce::dontSendNotification);
        strobe_.setToggleState(s.show.strobingDisabled, juce::dontSendNotification);
        strobeNote_.setText(s.show.strobingDisabled ? "Strobing is locked off." : "Strobing is allowed. Flash scenes can run.",
                            juce::dontSendNotification);
        syncOutputs(s);
        displayNote_.setText(displayList_.size() < 2 ? "Only one display found. Audience output needs a second display." : juce::String(
                             std::to_string(displayList_.size()) + " displays found. Pick one, then open an audience output on it."),
                             juce::dontSendNotification);
    }

    void layout(int width) {
        int y = 8;
        auto row = [&](int h, std::initializer_list<std::pair<juce::Component*, int>> items) {
            uikit::placeRow(juce::Rectangle<int>(8, y, width - 16, h), items);
            y += h + kGap;
        };
        auto head = [&](juce::Label& l) { l.setBounds(8, y, width - 16, kHeaderH); y += kHeaderH + 2; };
        auto full = [&](juce::Component& c, int h) { c.setBounds(8, y, width - 16, h); y += h + kGap; };

        row(kRowH, {{&runWizard_, 200}});
        y += 6;
        head(audioHead_);
        full(audioState_, 22);
        row(kRowH, {{&deviceType_, 130}, {&deviceName_, 0}, {&applyDevice_, 130}});
        row(kRowH, {{&rate_, 130}, {&rateNote_, 0}});
        y += 6;
        head(displayHead_);
        full(displayNote_, 22);
        row(kRowH, {{&displayBox_, 0}, {&refreshDisplays_, 140}, {&identify_, 160}});
        for (auto& r : rows_) {
            if (!r.name->isVisible()) continue;
            uikit::placeRow(juce::Rectangle<int>(8, y, width - 16, kRowH), {{r.name.get(), 0}, {r.overlay.get(), 180}, {r.open.get(), 150}, {r.close.get(), 90}});
            y += kRowH + kGap;
        }
        y += 6;
        head(safetyHead_);
        row(kRowH, {{&strobe_, 180}, {&strobeNote_, 0}});
        y += 6;
        head(appearanceHead_);
        row(kRowH, {{&theme_, 180}, {&clean_, 130}, {&reduce_, 150}, {&appearanceNote_, 0}});
        y += 6;
        head(filesHead_);
        row(kRowH, {{&save_, 150}, {&saveAs_, 150}, {&open_, 150}});
        y += 6;
        head(benchHead_);
        full(benchNote_, 22);
        full(notice_, 24);
        height_ = y + 12;
        setSize(width, height_);
    }

    int height() const { return height_; }

private:
    struct OutputRow {
        OutputId id = 0;
        std::unique_ptr<juce::Label> name;
        std::unique_ptr<juce::TextButton> open, close;
        std::unique_ptr<juce::ToggleButton> overlay;
    };

    static juce::String audioText(const UiSnapshot& s) {
        juce::String t = "Audio: ";
        t << juce::String(adapterStateName(s.audio.deviceState));
        if (s.audio.rateHz > 0) t << ", " << juce::String(static_cast<int>(s.audio.rateHz)) << " Hz";
        if (!s.audio.deviceError.empty()) t << ". " << juce::String(s.audio.deviceError);
        return t;
    }

    void refreshDisplayList() {
        displayList_ = juce::Desktop::getInstance().getDisplays().displays;
        displayBox_.clear(juce::dontSendNotification);
        for (int i = 0; i < displayList_.size(); ++i) {
            const auto& d = displayList_.getReference(i);
            displayBox_.addItem("Display " + juce::String(i + 1) + " (" + juce::String(static_cast<int>(d.logicalBounds.getWidth())) + "x" +
                                juce::String(static_cast<int>(d.logicalBounds.getHeight())) + ")", i + 1);
        }
        if (displayList_.size() > 0) displayBox_.setSelectedId(1, juce::dontSendNotification);
    }

    void syncOutputs(const UiSnapshot& s) {
        int shown = 0;
        for (auto& r : rows_) { r.name->setVisible(false); r.open->setVisible(false); r.close->setVisible(false); r.overlay->setVisible(false); }
        for (const auto& o : s.outputs) {
            if (shown >= kMaxOutputRows) break;
            auto& r = rows_[static_cast<std::size_t>(shown++)];
            r.id = o.id;
            const bool audience = o.kind == RouteKind::Audience;
            juce::String label = o.label.empty() ? juce::String("Output " + std::to_string(o.id)) : juce::String(o.label);
            r.name->setText(label + (audience ? " (audience)" : " (controls)") + ", " + juce::String(o.width) + "x" + juce::String(o.height),
                            juce::dontSendNotification);
            r.name->setVisible(true);
            r.open->setVisible(audience);
            r.close->setVisible(audience);
            r.open->setEnabled(!outputs_.isOpen(o.id));
            r.close->setEnabled(outputs_.isOpen(o.id));
            r.overlay->setVisible(true);
            r.overlay->setToggleState(false, juce::dontSendNotification);
        }
    }

    void openRow(int i) {
        auto& r = rows_[static_cast<std::size_t>(i)];
        const int idx = displayBox_.getSelectedId() - 1;
        if (idx < 0 || idx >= displayList_.size()) { setNotice("Pick a display first."); return; }
        juce::String reason;
        if (!outputs_.open(r.id, displayList_.getReference(idx), &top_, reason)) setNotice(reason);
        else setNotice("Audience output open. Close it here to stop.");
    }

    void closeRow(int i) {
        outputs_.close(rows_[static_cast<std::size_t>(i)].id);
        setNotice("Audience output closed.");
    }

    void chooseFile(bool save) {
        chooser_ = std::make_unique<juce::FileChooser>(save ? "Save a copy of the show" : "Open a show",
                                                       juce::File(), "*.stagesignal", true);
        const int mode = save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting
                              : juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        chooser_->launchAsync(mode, [this, save](const juce::FileChooser& fc) {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            AppCommand c;
            c.kind = save ? AppCommandKind::SaveShowFile : AppCommandKind::LoadShowFile;
            c.text = f.getFullPathName().toStdString();
            if (!model_.postCommand(c)) setNotice("Too many requests. Try again in a moment.");
        });
    }

public:
private:
    UiModel& model_;
    audience::OutputManager& outputs_;
    juce::Component& top_;
    juce::Array<juce::Displays::Display> displayList_;
    std::array<OutputRow, kMaxOutputRows> rows_;
    std::unique_ptr<juce::FileChooser> chooser_;
    int height_ = 0;

    juce::TextButton runWizard_;
    juce::Label audioHead_, audioState_, rateNote_, displayHead_, displayNote_, safetyHead_, strobeNote_,
        appearanceHead_, appearanceNote_, filesHead_, benchHead_, benchNote_, notice_;
    juce::ComboBox deviceType_, rate_, displayBox_, theme_;
    juce::TextEditor deviceName_;
    juce::TextButton applyDevice_, refreshDisplays_, identify_, save_, saveAs_, open_;
    juce::ToggleButton strobe_, clean_, reduce_;
};

SetupPanel::SetupPanel(UiModel& model) : PanelBase(model, "Setup"), outputs_(model) {
    content_ = std::make_unique<Content>(model, outputs_, *this);
    content_->onWizard = [this] { runWizard(); };
    viewport_.setViewedComponent(content_.get(), false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
    setWantsKeyboardFocus(true);
}

SetupPanel::~SetupPanel() {
    wizard_.reset();
    viewport_.setViewedComponent(nullptr, false);
    content_.reset();
}

void SetupPanel::paint(juce::Graphics& g) { g.fillAll(theme::bgNight()); }

void SetupPanel::resized() {
    viewport_.setBounds(getLocalBounds());
    content_->layout(getWidth() - 14);
    if (wizard_) wizard_->setBounds(getLocalBounds());
}

bool SetupPanel::keyPressed(const juce::KeyPress& key) { return live::handleShortcut(model_, key); }

void SetupPanel::runWizard() {
    wizard_ = std::make_unique<setup::Wizard>(
        model_,
        [this] { if (wizard_) wizard_->setVisible(false); },   // finish: the wizard keeps its summary
        [this] { if (wizard_) wizard_->setVisible(false); });  // discard
    addAndMakeVisible(*wizard_);
    viewport_.setVisible(false);
    wizard_->setBounds(getLocalBounds());
    wizard_->setVisible(true);
    wizard_->grabKeyboardFocus();
}

juce::Component* SetupPanel::control(const juce::String& componentId) {
    if (wizard_ && wizard_->isVisible()) return uikit::findById(wizard_.get(), componentId);
    return content_->control(componentId);
}

void SetupPanel::uiSnapshotChanged(const UiSnapshot& s) {
    if (wizard_) wizard_->observe(s);
    if (wizard_ && wizard_->isVisible()) return;
    viewport_.setVisible(true);
    content_->apply(s);
}

void SetupPanel::uiCommandOutcome(const CommandOutcome& o) {
    if (o.message.empty()) return;
    content_->setNotice(juce::String(o.message));
}

}  // namespace ss::app
