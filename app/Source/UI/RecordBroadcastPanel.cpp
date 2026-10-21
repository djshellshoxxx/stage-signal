#include "UI/RecordBroadcastPanel.h"
#include <cstdlib>
#include <cstdio>
#include <initializer_list>
#include "UI/Overlays/OverlayText.h"
#include "UI/Theme.h"

namespace ss::app {

namespace {
const char* recordWord(RecordUiState s) {
    switch (s) {
        case RecordUiState::Off: return "Idle";
        case RecordUiState::Preparing: return "Preparing";
        case RecordUiState::Recording: return "Recording";
        case RecordUiState::Stopping: return "Stopping";
        case RecordUiState::Recoverable: return "Recoverable. Check the last file before you record again.";
        case RecordUiState::Failed: return "Failed. See the reason below.";
    }
    return "Idle";
}

const char* broadcastWord(BroadcastUiState s) {
    switch (s) {
        case BroadcastUiState::Off: return "Off";
        case BroadcastUiState::Preparing: return "Preparing";
        case BroadcastUiState::Live: return "Live";
        case BroadcastUiState::Reconnecting: return "Reconnecting";
        case BroadcastUiState::Stopping: return "Stopping";
        case BroadcastUiState::Failed: return "Error. See the reason below.";
    }
    return "Off";
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

RecordBroadcastPanel::RecordBroadcastPanel(UiModel& model) : PanelBase(model, "Record / Broadcast") {
    styleLabel(recTitle_, true);
    recTitle_.setText("Recording", juce::dontSendNotification);
    styleLabel(recChip_, true);
    styleLabel(recSource_, false);
    styleLabel(recFormat_, false);
    recFormat_.setText("Format profile and dynamics: not reported to the UI yet.", juce::dontSendNotification);
    styleLabel(recFolder_, false);
    recFolder_.setText("No folder chosen", juce::dontSendNotification);
    styleLabel(recElapsed_, false);
    styleLabel(recNote_, false);
    styleLabel(recStatus_, false);

    chooseFolder_.onClick = [this] {
        folderPicker_.choose([this](const juce::File& f) {
            AppCommand c;
            c.kind = AppCommandKind::SetRecordFolder;
            c.text = f.getFullPathName().toStdString();
            if (overlay::sendCommand(model_, c, &folderCorrelation_)) {
                recFolder_.setText(f.getFileName(), juce::dontSendNotification);
                recStatus_.setText("Sent.", juce::dontSendNotification);
            } else {
                recStatus_.setText("Too many requests waiting. Try again in a moment.", juce::dontSendNotification);
            }
        });
    };
    startRec_.onClick = [this] {
        recStatus_.setText(juce::String(overlay::sendAction(model_, ss::ActionId::StartRecording, 0.0, kAllOutputs,
                                                            &recCorrelation_)),
                           juce::dontSendNotification);
    };
    stopRec_.onClick = [this] {
        recStatus_.setText(juce::String(overlay::sendAction(model_, ss::ActionId::StopRecording, 0.0, kAllOutputs,
                                                            &recCorrelation_)),
                           juce::dontSendNotification);
    };
    marker_.onClick = [this] {
        recStatus_.setText(juce::String(overlay::sendAction(model_, ss::ActionId::AddMarker, 0.0, kAllOutputs,
                                                            &recCorrelation_)),
                           juce::dontSendNotification);
    };

    styleLabel(castTitle_, true);
    castTitle_.setText("Broadcast", juce::dontSendNotification);
    styleLabel(castChip_, true);
    styleLabel(hostLabel_, false);
    hostLabel_.setText("Host, port and path", juce::dontSendNotification);
    styleEditor(host_, "Host");
    styleEditor(port_, "Port");
    styleEditor(path_, "Path");
    styleLabel(keyLabel_, false);
    keyLabel_.setText("Stream key", juce::dontSendNotification);
    styleEditor(key_, "Stream key");
    key_.setPasswordCharacter('*');
    styleLabel(keyNote_, false);
    keyNote_.setText("The key is masked and is never shown again after saving.", juce::dontSendNotification);
    styleLabel(settingsNote_, false);
    settingsNote_.setText("Host, port and path are saved on this computer. The key goes to the system keychain.", juce::dontSendNotification);
    saveSettings_.setTooltip("Save the endpoint and the stream key.");
    saveSettings_.onClick = [this] {
        AppCommand c;
        c.kind = AppCommandKind::SetBroadcastEndpoint;
        c.text = host_.getText().toStdString();
        c.text2 = path_.getText().toStdString();
        c.a = std::atoi(port_.getText().toRawUTF8());
        std::uint64_t corr = 0;
        if (!overlay::sendCommand(model_, c, &corr)) { castStatus_.setText("Too many requests waiting. Try again in a moment.", juce::dontSendNotification); return; }
        if (key_.getText().isNotEmpty()) {
            AppCommand k;
            k.kind = AppCommandKind::SetBroadcastKey;
            k.text = key_.getText().toStdString();
            overlay::sendCommand(model_, k, &corr);
            key_.clear();                                  // the key is never kept in the editor after saving
        }
        castStatus_.setText("Saved.", juce::dontSendNotification);
    };
    styleLabel(summary_, false);
    styleLabel(castNote_, false);
    styleLabel(castStats_, false);
    styleLabel(castStatus_, false);
    styleLabel(confirmText_, false);
    confirmText_.setColour(juce::Label::textColourId, theme::stateWarn());

    startCast_.onClick = [this] { showBroadcastConfirm(true); };
    confirmStart_.onClick = [this] {
        showBroadcastConfirm(false);
        castStatus_.setText(juce::String(overlay::sendAction(model_, ss::ActionId::StartBroadcast, 0.0, kAllOutputs,
                                                             &castCorrelation_)),
                            juce::dontSendNotification);
    };
    cancelStart_.onClick = [this] { showBroadcastConfirm(false); };
    stopCast_.onClick = [this] {
        castStatus_.setText(juce::String(overlay::sendAction(model_, ss::ActionId::StopBroadcast, 0.0, kAllOutputs,
                                                             &castCorrelation_)),
                            juce::dontSendNotification);
    };
    showBroadcastConfirm(false);

    for (auto* c : std::initializer_list<juce::Component*>{
             &recTitle_, &recChip_, &recSource_, &recFormat_, &recFolder_, &chooseFolder_, &startRec_, &stopRec_, &marker_,
             &recElapsed_, &recNote_, &recStatus_, &castTitle_, &castChip_, &hostLabel_, &host_, &port_, &path_, &keyLabel_,
             &key_, &keyNote_, &settingsNote_, &saveSettings_, &summary_, &startCast_, &stopCast_, &confirmText_,
             &confirmStart_, &cancelStart_, &castNote_, &castStats_, &castStatus_})
        addAndMakeVisible(c);
}

RecordBroadcastPanel::~RecordBroadcastPanel() = default;

void RecordBroadcastPanel::paint(juce::Graphics& g) {
    g.fillAll(theme::bgPanel());
    g.setColour(theme::lineDivider());
    g.drawVerticalLine(getWidth() / 2, 8.0f, static_cast<float>(getHeight() - 8));
}

void RecordBroadcastPanel::showBroadcastConfirm(bool show) {
    castConfirmOpen_ = show;
    confirmText_.setVisible(show);
    confirmStart_.setVisible(show);
    cancelStart_.setVisible(show);
    if (show) {
        if (auto s = model_.snapshot()) confirmText_.setText(juce::String(publicationSummary(*s)), juce::dontSendNotification);
        cancelStart_.grabKeyboardFocus();   // focus starts on Cancel (app-ui 15)
    }
    resized();
}

std::string RecordBroadcastPanel::publicationSummary(const UiSnapshot& s) const {
    return "Audience output: " + overlay::outputLabel(s, s.show.selectedOutput) +
           ". Audio: processed bus. Camera: not published.";
}

void RecordBroadcastPanel::resized() {
    const int lx = 8, rx = getWidth() / 2 + 8, colW = getWidth() / 2 - 16;
    auto row = [](juce::Component& c, int x, int y, int w, int h) { c.setBounds(x, y, w, h); };
    int y = 8;
    row(recTitle_, lx, y, colW, 22); y += 26;
    row(recChip_, lx, y, colW, 22); y += 24;
    row(recSource_, lx, y, colW, 20); y += 22;
    row(recFormat_, lx, y, colW, 20); y += 22;
    row(chooseFolder_, lx, y, 120, 26);
    row(recFolder_, lx + 128, y, colW - 128, 26); y += 32;
    row(startRec_, lx, y, 130, 30);
    row(stopRec_, lx + 138, y, 130, 30);
    row(marker_, lx + 276, y, colW - 276, 30); y += 36;
    row(recElapsed_, lx, y, colW, 20); y += 24;
    row(recNote_, lx, y, colW, 36); y += 40;
    row(recStatus_, lx, y, colW, 22);

    y = 8;
    row(castTitle_, rx, y, colW, 22); y += 26;
    row(castChip_, rx, y, colW, 22); y += 24;
    row(hostLabel_, rx, y, colW, 20); y += 22;
    row(host_, rx, y, 200, 26);
    row(port_, rx + 208, y, 60, 26);
    row(path_, rx + 276, y, colW - 276, 26); y += 32;
    row(keyLabel_, rx, y, colW, 20); y += 22;
    row(key_, rx, y, 260, 26);
    row(saveSettings_, rx + 268, y, colW - 268, 26); y += 30;
    row(keyNote_, rx, y, colW, 20); y += 22;
    row(settingsNote_, rx, y, colW, 20); y += 24;
    row(summary_, rx, y, colW, 36); y += 40;
    if (castConfirmOpen_) {
        row(confirmText_, rx, y, colW, 36); y += 40;
        row(confirmStart_, rx, y, 110, 28);
        row(cancelStart_, rx + 118, y, 100, 28); y += 34;
    } else {
        row(startCast_, rx, y, 140, 30);
        row(stopCast_, rx + 148, y, 140, 30); y += 36;
    }
    row(castStats_, rx, y, colW, 20); y += 22;
    row(castNote_, rx, y, colW, 36); y += 40;
    row(castStatus_, rx, y, colW, 22);
}

void RecordBroadcastPanel::uiSnapshotChanged(const UiSnapshot& s) {
    const auto& r = s.record;
    recordingActive_ = r.state == RecordUiState::Recording || r.state == RecordUiState::Preparing;
    recChip_.setText(juce::String(recordWord(r.state)), juce::dontSendNotification);
    recNote_.setText(juce::String(r.note), juce::dontSendNotification);
    const auto secs = r.elapsedMs / 1000;
    char buf[160];
    std::snprintf(buf, sizeof buf, "Elapsed %02llu:%02llu:%02llu, %.1f MB written",
                  static_cast<unsigned long long>(secs / 3600), static_cast<unsigned long long>((secs / 60) % 60),
                  static_cast<unsigned long long>(secs % 60), static_cast<double>(r.bytesWritten) / (1024.0 * 1024.0));
    recElapsed_.setText(juce::String(buf), juce::dontSendNotification);
    startRec_.setEnabled(!recordingActive_ && r.state != RecordUiState::Stopping);
    stopRec_.setEnabled(recordingActive_);
    marker_.setEnabled(r.state == RecordUiState::Recording);
    stopRec_.setTooltip(recordingActive_ ? "Stop recording." : "Nothing is recording.");
    recSource_.setText("Source: " + juce::String(overlay::outputLabel(s, s.show.selectedOutput)) +
                           ". Change it on Live.",
                       juce::dontSendNotification);

    {
        const auto& e = s.broadcastEndpoint;
        if (!host_.hasKeyboardFocus(true) && host_.getText().isEmpty()) host_.setText(juce::String(e.host), false);
        if (!path_.hasKeyboardFocus(true) && path_.getText().isEmpty()) path_.setText(juce::String(e.path), false);
        if (!port_.hasKeyboardFocus(true) && port_.getText().isEmpty() && e.port > 0) port_.setText(juce::String(e.port), false);
        keyNote_.setText(e.keySaved ? "A key is saved. It is never shown again. Type a new one to replace it." : "The key is masked and is never shown again after saving.", juce::dontSendNotification);
    }
    const auto& b = s.broadcast;
    broadcastActive_ = b.state == BroadcastUiState::Live || b.state == BroadcastUiState::Reconnecting ||
                       b.state == BroadcastUiState::Preparing;
    castChip_.setText(juce::String(broadcastWord(b.state)), juce::dontSendNotification);
    castNote_.setText(juce::String(b.note), juce::dontSendNotification);
    std::snprintf(buf, sizeof buf, "Bitrate %u kbps, %llu frames dropped", b.bitrateKbps,
                  static_cast<unsigned long long>(b.droppedFrames));
    castStats_.setText(juce::String(buf), juce::dontSendNotification);
    startCast_.setEnabled(!broadcastActive_);
    stopCast_.setEnabled(broadcastActive_);
    if (castConfirmOpen_ && broadcastActive_) showBroadcastConfirm(false);
    summary_.setText(juce::String(publicationSummary(s)), juce::dontSendNotification);
}

void RecordBroadcastPanel::uiActionOutcome(const ActionOutcome& o) {
    if (o.accepted) return;
    const juce::String text(o.reason.empty() ? "Not done. Try again." : o.reason);
    if (o.correlationId != 0 && o.correlationId == recCorrelation_) {
        recCorrelation_ = 0;
        recStatus_.setText(text, juce::dontSendNotification);
    } else if (o.correlationId != 0 && o.correlationId == castCorrelation_) {
        castCorrelation_ = 0;
        castStatus_.setText(text, juce::dontSendNotification);
    }
}

void RecordBroadcastPanel::uiCommandOutcome(const CommandOutcome& o) {
    if (o.correlationId == 0 || o.correlationId != folderCorrelation_) return;
    folderCorrelation_ = 0;
    if (!o.ok) recStatus_.setText(juce::String(o.message.empty() ? "Folder not set. Pick another folder." : o.message),
                                  juce::dontSendNotification);
}

}  // namespace ss::app
