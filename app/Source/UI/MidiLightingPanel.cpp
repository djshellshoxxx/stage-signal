#include "UI/MidiLightingPanel.h"
#include <cstdlib>
#include "UI/Overlays/OverlayText.h"
#include "UI/Theme.h"

namespace ss::app {

namespace {
const char* lightingWord(LightingUiState s) {
    switch (s) {
        case LightingUiState::Disarmed: return "Disarmed";
        case LightingUiState::Armed: return "Armed. Lights are live.";
        case LightingUiState::DryRun: return "Dry run. Nothing is sent.";
        case LightingUiState::Fault: return "Fault. Arm again after you fix the cause.";
    }
    return "Disarmed";
}

void styleLabel(juce::Label& l, bool primary) {
    l.setFont(overlay::bodyFont(theme::kBodyFont));
    l.setColour(juce::Label::textColourId, primary ? theme::textPrimary() : theme::textSecondary());
}

void styleEditor(juce::TextEditor& e, const char* placeholder) {
    e.setTextToShowWhenEmpty(placeholder, theme::textMuted());
    e.setFont(overlay::bodyFont(theme::kBodyFont));
    e.setColour(juce::TextEditor::backgroundColourId, theme::bgSunken());
    e.setColour(juce::TextEditor::textColourId, theme::textPrimary());
    e.setColour(juce::TextEditor::outlineColourId, theme::lineControl());
}
}  // namespace

MidiLightingPanel::MidiLightingPanel(UiModel& model) : PanelBase(model, "MIDI / Lighting") {
    styleLabel(midiTitle_, true);
    midiTitle_.setText("MIDI", juce::dontSendNotification);
    styleLabel(portsText_, false);
    styleLabel(learnStatus_, false);
    styleLabel(mappingHeader_, true);
    mappingHeader_.setText("Device, channel, type, number, range, mode, action", juce::dontSendNotification);
    styleLabel(mappingEmpty_, false);
    mappingEmpty_.setText("No mappings yet. Learn a control to add one.", juce::dontSendNotification);
    styleLabel(alarmsText_, false);
    alarmsText_.setText("Feedback alarms: none reported.", juce::dontSendNotification);
    styleLabel(midiNote_, false);

    learn_.onClick = [this] {
        learning_ = !learning_;
        learn_.setButtonText(learning_ ? "Cancel learn" : "Learn");
        learnStatus_.setText(learning_
                                 ? "Waiting for a MIDI message. Move a control. Nothing is captured until MIDI input reports to the UI."
                                 : "",
                             juce::dontSendNotification);
    };
    clock_.setEnabled(false);
    clock_.setTooltip("Clock output needs a tempo source choice that is not in this build yet.");
    sysEx_.setEnabled(false);
    sysEx_.setTooltip("SysEx stays off in this build.");
    midiOut_.onClick = [this] {
        AppCommand c;
        c.kind = midiOut_.getToggleState() ? AppCommandKind::EnableMidiOutput : AppCommandKind::DisableMidiOutput;
        overlay::sendCommand(model_, c, &lastCommandCorrelation_);
    };

    styleLabel(lightTitle_, true);
    lightTitle_.setText("Lighting", juce::dontSendNotification);
    styleLabel(stateChip_, true);
    styleLabel(lightNote_, false);
    styleLabel(destLabel_, false);
    destLabel_.setText("Destination IPv4 and port", juce::dontSendNotification);
    styleLabel(universeText_, false);
    universeText_.setText("Universe and fixtures: from the show. Rate 25 per second, up to 40.", juce::dontSendNotification);
    styleLabel(lightStatus_, false);
    styleLabel(confirmText_, false);
    confirmText_.setColour(juce::Label::textColourId, theme::stateWarn());
    styleLabel(patchText_, false);
    patchText_.setText("Fixture patch: not reported to the UI yet. Conflicts show in Diagnostics.", juce::dontSendNotification);

    styleEditor(interfaceIp_, "Interface IPv4");
    interfaceIp_.setInputRestrictions(15, "0123456789.");
    styleEditor(destIp_, "Destination IPv4");
    destIp_.setInputRestrictions(15, "0123456789.");
    styleEditor(destPort_, "Port");
    destPort_.setInputRestrictions(5, "0123456789");

    useInterface_.onClick = [this] {
        AppCommand c;
        c.kind = AppCommandKind::SelectLightingInterface;
        c.text = interfaceIp_.getText().toStdString();
        if (c.text.empty()) { setLightingStatus("Enter the interface address first."); return; }
        overlay::sendCommand(model_, c, &lastCommandCorrelation_);
        setLightingStatus("Sent.");
    };
    setDest_.onClick = [this] {
        AppCommand c;
        c.kind = AppCommandKind::SetLightingDestination;
        c.text = destIp_.getText().toStdString();
        c.a = std::atoi(destPort_.getText().toRawUTF8());
        if (c.text.empty()) { setLightingStatus("Enter the destination address first."); return; }
        overlay::sendCommand(model_, c, &lastCommandCorrelation_);
        setLightingStatus("Sent.");
    };
    broadcast_.setEnabled(false);
    broadcast_.setTooltip("Broadcast delivery is not available in this build.");

    arm_.onClick = [this] {
        if (destIp_.getText().isEmpty()) { setLightingStatus("Set a destination before you arm."); return; }
        showArmConfirm(true);
    };
    confirmArm_.onClick = [this] {
        showArmConfirm(false);
        setLightingStatus(juce::String(overlay::sendAction(model_, ss::ActionId::ArmLighting, 0.0, kAllOutputs,
                                                           &lastActionCorrelation_)));
    };
    cancelArm_.onClick = [this] { showArmConfirm(false); };
    disarm_.onClick = [this] {
        setLightingStatus(juce::String(overlay::sendAction(model_, ss::ActionId::DisarmLighting, 0.0, kAllOutputs,
                                                           &lastActionCorrelation_)));
    };
    dryRun_.onClick = [this] {
        setLightingStatus(juce::String(overlay::sendAction(model_, ss::ActionId::LightingDryRun,
                                                           dryRun_.getToggleState() ? 1.0 : 0.0, kAllOutputs,
                                                           &lastActionCorrelation_)));
    };
    blackout_.onClick = [this] {
        setLightingStatus(juce::String(overlay::sendAction(model_, ss::ActionId::LightingBlackout, 0.0, kAllOutputs,
                                                           &lastActionCorrelation_)));
    };
    restore_.onClick = [this] {
        setLightingStatus(juce::String(overlay::sendAction(model_, ss::ActionId::RestoreBlackout, 0.0, kAllOutputs,
                                                           &lastActionCorrelation_)));
    };
    showArmConfirm(false);

    for (auto* c : std::initializer_list<juce::Component*>{
             &midiTitle_, &portsText_, &learn_, &learnStatus_, &mappingHeader_, &mappingEmpty_, &clock_, &sysEx_,
             &midiOut_, &alarmsText_, &midiNote_, &lightTitle_, &stateChip_, &lightNote_, &interfaceIp_, &useInterface_,
             &destLabel_, &destIp_, &destPort_, &setDest_, &broadcast_, &universeText_,
             &arm_, &disarm_, &confirmText_, &confirmArm_, &cancelArm_, &dryRun_, &blackout_, &restore_, &patchText_,
             &lightStatus_})
        addAndMakeVisible(c);
}

MidiLightingPanel::~MidiLightingPanel() = default;

void MidiLightingPanel::paint(juce::Graphics& g) {
    g.fillAll(theme::bgPanel());
    g.setColour(theme::lineDivider());
    g.drawVerticalLine(getWidth() / 2, 8.0f, static_cast<float>(getHeight() - 8));
}

void MidiLightingPanel::setLightingStatus(const juce::String& text) {
    lightStatus_.setText(text, juce::dontSendNotification);
}

void MidiLightingPanel::showArmConfirm(bool show) {
    armConfirmOpen_ = show;
    confirmText_.setVisible(show);
    confirmArm_.setVisible(show);
    cancelArm_.setVisible(show);
    if (show) {
        const auto ip = interfaceIp_.getText().isEmpty() ? juce::String("the default interface") : interfaceIp_.getText();
        confirmText_.setText("Arm lighting to " + destIp_.getText() + " on " + ip + "? Lights will respond.",
                             juce::dontSendNotification);
        cancelArm_.grabKeyboardFocus();   // focus starts on Cancel (app-ui 15)
    }
    resized();
}

void MidiLightingPanel::resized() {
    const int lx = 8, rx = getWidth() / 2 + 8, colW = getWidth() / 2 - 16;
    int y = 8;
    auto row = [&](juce::Component& c, int x, int yy, int w, int h) { c.setBounds(x, yy, w, h); };
    // MIDI column
    row(midiTitle_, lx, y, colW, 22); y += 26;
    row(portsText_, lx, y, colW, 34); y += 36;
    row(learn_, lx, y, 110, 28);
    row(learnStatus_, lx + 118, y, colW - 118, 28); y += 34;
    row(mappingHeader_, lx, y, colW, 20); y += 22;
    row(mappingEmpty_, lx, y, colW, 22); y += 28;
    row(clock_, lx, y, colW, 24); y += 26;
    row(sysEx_, lx, y, colW, 24); y += 26;
    row(midiOut_, lx, y, colW, 24); y += 30;
    row(alarmsText_, lx, y, colW, 22); y += 24;
    row(midiNote_, lx, y, colW, 22);
    // Lighting column
    y = 8;
    row(lightTitle_, rx, y, colW, 22); y += 26;
    row(stateChip_, rx, y, colW, 22); y += 24;
    row(lightNote_, rx, y, colW, 20); y += 24;
    row(interfaceIp_, rx, y, 170, 26);
    row(useInterface_, rx + 178, y, colW - 178, 26); y += 30;
    row(destLabel_, rx, y, colW, 20); y += 22;
    row(destIp_, rx, y, 170, 26);
    row(destPort_, rx + 178, y, 70, 26);
    row(setDest_, rx + 256, y, colW - 256, 26); y += 30;
    row(broadcast_, rx, y, colW, 24); y += 26;
    row(universeText_, rx, y, colW, 22); y += 24;
    row(arm_, rx, y, 90, 28);
    row(disarm_, rx + 98, y, 90, 28);
    row(restore_, rx + colW - 90, y, 90, 28);
    row(blackout_, rx + colW - 196, y, 96, 28); y += 34;
    if (armConfirmOpen_) {
        row(confirmText_, rx, y, colW, 20); y += 22;
        row(confirmArm_, rx, y, 100, 28);
        row(cancelArm_, rx + 108, y, 90, 28); y += 34;
    }
    row(dryRun_, rx, y, colW, 24); y += 28;
    row(patchText_, rx, y, colW, 34); y += 36;
    row(lightStatus_, rx, y, colW, 22);
}

void MidiLightingPanel::uiSnapshotChanged(const UiSnapshot& s) {
    const auto& m = s.midi;
    portsText_.setText("Input ports: " + juce::String(static_cast<int>(m.inputPorts)) + ", output ports: " +
                           juce::String(static_cast<int>(m.outputPorts)) + ". Port names are not reported yet.",
                       juce::dontSendNotification);
    midiNote_.setText(juce::String(m.note), juce::dontSendNotification);
    midiOut_.setToggleState(m.outputEnabled, juce::dontSendNotification);
    clock_.setToggleState(m.clockOn, juce::dontSendNotification);

    const auto& l = s.lighting;
    stateChip_.setText(juce::String(lightingWord(l.state)), juce::dontSendNotification);
    lightNote_.setText(juce::String(l.note), juce::dontSendNotification);
    dryRun_.setToggleState(l.state == LightingUiState::DryRun, juce::dontSendNotification);
    arm_.setEnabled(l.state != LightingUiState::Armed);
    disarm_.setEnabled(l.state == LightingUiState::Armed || l.state == LightingUiState::DryRun);
    arm_.setTooltip(l.state == LightingUiState::Armed ? "Lighting is already armed." : "Arm lighting. Asks you to confirm.");
    restore_.setEnabled(l.blackout);
    restore_.setTooltip(l.blackout ? "Restore lighting output." : "Lighting is not in blackout.");
    if (armConfirmOpen_ && l.state == LightingUiState::Armed) showArmConfirm(false);
}

void MidiLightingPanel::uiActionOutcome(const ActionOutcome& o) {
    if (lastActionCorrelation_ == 0 || o.correlationId != lastActionCorrelation_) return;
    lastActionCorrelation_ = 0;
    if (!o.accepted) setLightingStatus(juce::String(o.reason.empty() ? "Not done. Try again." : o.reason));
}

void MidiLightingPanel::uiCommandOutcome(const CommandOutcome& o) {
    if (lastCommandCorrelation_ == 0 || o.correlationId != lastCommandCorrelation_) return;
    lastCommandCorrelation_ = 0;
    if (!o.ok) setLightingStatus(juce::String(o.message.empty() ? "Not done. Try again." : o.message));
}

}  // namespace ss::app
