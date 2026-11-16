#include "UI/Overlays/CommandPalette.h"
#include <algorithm>
#include "UI/Overlays/OverlayText.h"
#include "UI/Theme.h"

namespace ss::app {

namespace {
std::string confirmQuestion(ss::ActionId id) {
    if (id == ss::ActionId::StartRecording) return "Start recording now?";
    if (id == ss::ActionId::StartBroadcast) return "Start broadcast now? The audience output goes live.";
    if (id == ss::ActionId::ArmLighting) return "Arm lighting now? Lights will respond.";
    return "Run this command now?";
}
}  // namespace

bool CommandPaletteOverlay::needsConfirmation(ss::ActionId id) {
    return id == ss::ActionId::StartRecording || id == ss::ActionId::StartBroadcast || id == ss::ActionId::ArmLighting;
}

CommandPaletteOverlay::CommandPaletteOverlay(UiModel& model)
    : model_(model), keymap_(palette::KeyMap::defaults()), palette_(&keymap_) {
    query_.setTextToShowWhenEmpty("Type a command", theme::textMuted());
    query_.setFont(overlay::bodyFont(theme::kBodyFont + 2.0f));
    query_.setColour(juce::TextEditor::backgroundColourId, theme::bgSunken());
    query_.setColour(juce::TextEditor::textColourId, theme::textPrimary());
    query_.setColour(juce::TextEditor::outlineColourId, theme::lineControl());
    query_.setColour(juce::TextEditor::focusedOutlineColourId, theme::focusRing());
    query_.onTextChange = [this] { refresh(); };
    query_.addKeyListener(this);

    argValue_.setFont(overlay::bodyFont(theme::kBodyFont));
    argValue_.setColour(juce::TextEditor::backgroundColourId, theme::bgSunken());
    argValue_.setColour(juce::TextEditor::textColourId, theme::textPrimary());
    argValue_.setColour(juce::TextEditor::outlineColourId, theme::lineControl());
    argValue_.setInputRestrictions(24, "0123456789.-%");
    argValue_.addKeyListener(this);
    argValue_.onReturnKey = [this] { commitArgument(); };
    argRun_.onClick = [this] { commitArgument(); };
    argCancel_.onClick = [this] { hideInlineRows(); query_.grabKeyboardFocus(); };
    confirmYes_.onClick = [this] {
        const auto id = pending_;
        hideInlineRows();
        submit(id, "");
    };
    confirmNo_.onClick = [this] { hideInlineRows(); query_.grabKeyboardFocus(); };

    list_.setModel(this);
    list_.setRowHeight(kRowH);
    list_.setColour(juce::ListBox::backgroundColourId, theme::bgPanel());
    list_.setMultipleSelectionEnabled(false);

    for (auto* l : {&status_, &argLabel_, &confirmLabel_}) {
        l->setFont(overlay::bodyFont(theme::kBodyFont));
        l->setColour(juce::Label::textColourId, theme::textSecondary());
    }
    confirmLabel_.setColour(juce::Label::textColourId, theme::stateWarn());

    addChildComponent(query_);
    addChildComponent(list_);
    addChildComponent(status_);
    addChildComponent(argLabel_);
    addChildComponent(argValue_);
    addChildComponent(argRun_);
    addChildComponent(argCancel_);
    addChildComponent(confirmLabel_);
    addChildComponent(confirmYes_);
    addChildComponent(confirmNo_);
    setVisible(false);
}

CommandPaletteOverlay::~CommandPaletteOverlay() { model_.removeListener(this); }

void CommandPaletteOverlay::open(juce::Component& host) {
    host.addAndMakeVisible(this);
    setBounds(host.getLocalBounds());
    toFront(false);
    model_.addListener(this);
    query_.setText("", false);
    hideInlineRows();
    setStatus("");
    setVisible(true);
    refresh();
    query_.grabKeyboardFocus();
}

void CommandPaletteOverlay::close() {
    if (!isVisible()) return;
    model_.removeListener(this);
    pendingCorrelation_ = 0;
    hideInlineRows();
    setVisible(false);
    if (auto* parent = getParentComponent()) parent->removeChildComponent(this);
    if (onClosed) onClosed();
}

void CommandPaletteOverlay::setQuery(const std::string& text) {
    query_.setText(juce::String(text), false);
    refresh();
}

void CommandPaletteOverlay::refresh() {
    palette::SearchOptions opt;
    opt.limit = 50;
    results_ = palette_.search(query_.getText().toStdString(), opt);
    // app-ui 12: safety-critical actions rank first for their name or keywords. The core ranks by score only, so
    // this keeps the core order inside each group.
    std::stable_partition(results_.begin(), results_.end(), [](const palette::SearchResult& r) {
        return r.safetyCritical && (r.field == palette::MatchField::Name || r.field == palette::MatchField::Keywords);
    });
    list_.updateContent();
    if (!results_.empty()) list_.selectRow(0);
    setStatus(results_.empty() ? "No command matches. Try fewer letters." : "");
}

void CommandPaletteOverlay::moveSelection(int delta) {
    if (results_.empty()) return;
    const int row = juce::jlimit(0, static_cast<int>(results_.size()) - 1, list_.getSelectedRow() + delta);
    list_.selectRow(row);
    list_.scrollToEnsureRowIsOnscreen(row);
}

void CommandPaletteOverlay::runRow(int row) {
    if (row < 0 || row >= static_cast<int>(results_.size())) return;
    const auto r = results_[static_cast<std::size_t>(row)];
    const auto* info = palette::Registry::find(r.id);
    if (info && info->value.takesValue) { showArgument(r); return; }
    if (needsConfirmation(r.id)) { showConfirm(r); return; }
    submit(r.id, "");
}

void CommandPaletteOverlay::showArgument(const palette::SearchResult& r) {
    const auto* info = palette::Registry::find(r.id);
    if (!info) return;
    pending_ = r.id;
    showingArg_ = true;
    showingConfirm_ = false;
    argLabel_.setText(juce::String(info->name) + ":", juce::dontSendNotification);
    argValue_.setText(juce::String(overlay::numberText(info->value.defaultValue)), false);
    const std::string unit = info->value.unit ? info->value.unit : "";
    setStatus("Range " + overlay::numberText(info->value.min) + " to " + overlay::numberText(info->value.max) +
              (unit.empty() ? "" : " " + unit));
    resized();
    argValue_.grabKeyboardFocus();
}

void CommandPaletteOverlay::showConfirm(const palette::SearchResult& r) {
    pending_ = r.id;
    showingConfirm_ = true;
    showingArg_ = false;
    confirmLabel_.setText(juce::String(confirmQuestion(r.id)), juce::dontSendNotification);
    resized();
    confirmYes_.grabKeyboardFocus();
}

void CommandPaletteOverlay::hideInlineRows() {
    showingArg_ = false;
    showingConfirm_ = false;
    pending_ = ss::ActionId::None;
    for (auto* c : {static_cast<juce::Component*>(&argLabel_), static_cast<juce::Component*>(&argValue_),
                    static_cast<juce::Component*>(&argRun_), static_cast<juce::Component*>(&argCancel_),
                    static_cast<juce::Component*>(&confirmLabel_), static_cast<juce::Component*>(&confirmYes_),
                    static_cast<juce::Component*>(&confirmNo_)})
        c->setVisible(false);
    if (isVisible()) resized();
}

bool CommandPaletteOverlay::submit(ss::ActionId id, const std::string& valueText) {
    auto made = palette::makeAction(id, valueText, ss::ActionSource::Ui);
    if (!made.ok()) {
        setStatus(juce::String(made.error().detail));
        return false;
    }
    const auto ticket = model_.submitAction(made.value());
    pendingCorrelation_ = ticket.correlationId;
    pending_ = id;
    if (!ticket.result.queued) {
        setStatus(juce::String(ticket.result.reason.empty() ? "Not sent. Try again." : ticket.result.reason));
        pendingCorrelation_ = 0;
    } else {
        setStatus("Sent.");
    }
    return true;
}

void CommandPaletteOverlay::commitArgument() {
    const auto id = pending_;
    if (id == ss::ActionId::None) return;
    if (submit(id, argValue_.getText().toStdString())) hideInlineRows();   // a bad value stays in the row
    else argValue_.grabKeyboardFocus();
}

void CommandPaletteOverlay::setStatus(const juce::String& text) {
    status_.setText(text, juce::dontSendNotification);
}

void CommandPaletteOverlay::uiActionOutcome(const ActionOutcome& o) {
    if (pendingCorrelation_ == 0 || o.correlationId != pendingCorrelation_) return;
    pendingCorrelation_ = 0;
    if (o.accepted) {
        palette_.noteUsed(static_cast<ss::ActionId>(o.actionId));
        // Closing removes this listener; defer so the listener list is not changed while it is being walked.
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<CommandPaletteOverlay>(this)] {
            if (safe != nullptr) safe->close();
        });
    } else {
        setStatus(juce::String(o.reason.empty() ? "Not run. Try again." : o.reason));
    }
}

int CommandPaletteOverlay::getNumRows() { return static_cast<int>(results_.size()); }

void CommandPaletteOverlay::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) {
    if (row < 0 || row >= static_cast<int>(results_.size())) return;
    const auto& r = results_[static_cast<std::size_t>(row)];
    const auto* info = palette::Registry::find(r.id);
    if (!info) return;
    g.fillAll(selected ? theme::bgRaised() : theme::bgPanel());
    g.setFont(overlay::bodyFont(theme::kBodyFont));
    g.setColour(theme::textPrimary());
    g.drawText(juce::String(info->name), 8, 0, w - 260, h, juce::Justification::centredLeft, true);
    std::string tags;
    if (r.safetyCritical) tags += "Safety  ";
    if (r.takesValue) tags += "Needs value  ";
    if (r.recent) tags += "Recent  ";
    tags += r.shortcut;
    g.setColour(theme::textSecondary());
    g.drawText(juce::String(tags), w - 252, 0, 244, h, juce::Justification::centredRight, true);
}

void CommandPaletteOverlay::listBoxItemClicked(int row, const juce::MouseEvent&) { runRow(row); }
void CommandPaletteOverlay::returnKeyPressed(int row) { runRow(row); }

bool CommandPaletteOverlay::keyPressed(const juce::KeyPress& key, juce::Component*) {
    if (key.isKeyCode(juce::KeyPress::escapeKey)) { close(); return true; }
    if (key.isKeyCode(juce::KeyPress::upKey)) { moveSelection(-1); return true; }
    if (key.isKeyCode(juce::KeyPress::downKey)) { moveSelection(1); return true; }
    if (key.isKeyCode(juce::KeyPress::returnKey)) {
        if (showingArg_) commitArgument();
        else if (!showingConfirm_) runRow(list_.getSelectedRow());
        return true;
    }
    return false;
}

juce::Rectangle<int> CommandPaletteOverlay::panelBounds() const {
    const int w = juce::jmin(kPanelW, getWidth() - 16);
    int h = 8 + 36 + 8 + kVisibleRows * kRowH + 8 + 22 + 8;
    if (showingArg_ || showingConfirm_) h += 36;
    return juce::Rectangle<int>((getWidth() - w) / 2, kTopOffset, w, h);
}

void CommandPaletteOverlay::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xb0000000));
    auto panel = panelBounds();
    g.setColour(theme::bgPanel());
    g.fillRect(panel);
    g.setColour(theme::lineControl());
    g.drawRect(panel, 1);
}

void CommandPaletteOverlay::resized() {
    auto p = panelBounds().reduced(8, 8);
    query_.setBounds(p.removeFromTop(36));
    p.removeFromTop(8);
    if (showingArg_ || showingConfirm_) {
        auto row = p.removeFromTop(32);
        if (showingArg_) {
            argLabel_.setBounds(row.removeFromLeft(140));
            argValue_.setBounds(row.removeFromLeft(120).reduced(0, 2));
            argRun_.setBounds(row.removeFromLeft(72).reduced(4, 2));
            argCancel_.setBounds(row.removeFromLeft(80).reduced(4, 2));
        } else {
            confirmYes_.setBounds(row.removeFromRight(96).reduced(4, 2));
            confirmNo_.setBounds(row.removeFromRight(80).reduced(4, 2));
            confirmLabel_.setBounds(row);
        }
        p.removeFromTop(4);
    }
    list_.setBounds(p.removeFromTop(kVisibleRows * kRowH));
    p.removeFromTop(8);
    status_.setBounds(p.removeFromTop(22));
    argLabel_.setVisible(showingArg_); argValue_.setVisible(showingArg_);
    argRun_.setVisible(showingArg_); argCancel_.setVisible(showingArg_);
    confirmLabel_.setVisible(showingConfirm_); confirmYes_.setVisible(showingConfirm_);
    confirmNo_.setVisible(showingConfirm_);
    query_.setVisible(true); list_.setVisible(true); status_.setVisible(true);
}

void CommandPaletteOverlay::mouseDown(const juce::MouseEvent& e) {
    if (!panelBounds().contains(e.getPosition())) close();
}

}  // namespace ss::app
