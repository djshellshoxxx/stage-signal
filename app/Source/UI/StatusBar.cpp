#include "UI/StatusBar.h"
#include "UI/Live/Shortcuts.h"

namespace ss::app {

namespace {
constexpr int kLive = 0, kSetup = 1, kMidiLighting = 3, kRecordBroadcast = 4, kDiagnostics = 5;
// Widths at 800 px (app-ui 4.1, adapted for the MIDI and Perf chips). Sum is 800.
constexpr int kWidths[StatusBar::kChipCount] = {88, 130, 76, 104, 80, 56, 70, 72, 60, 64};
}

StatusBar::StatusBar(UiModel& model) : PanelBase(model, "Status bar") {
    const int tabs[StatusBar::kChipCount] = {kSetup, kSetup, kLive, kLive, kLive, kMidiLighting, kMidiLighting,
                                             kRecordBroadcast, kRecordBroadcast, kDiagnostics};
    for (int i = 0; i < kChipCount; ++i) {
        tabFor_[static_cast<std::size_t>(i)] = tabs[i];
        auto& c = chips_[static_cast<std::size_t>(i)];
        c.onActivate = [this, i] { if (onJumpToTab) onJumpToTab(tabFor_[static_cast<std::size_t>(i)]); };
        addAndMakeVisible(c);
    }
}

StatusBar::~StatusBar() = default;

void StatusBar::paint(juce::Graphics& g) { g.fillAll(theme::bgPanel()); }

bool StatusBar::keyPressed(const juce::KeyPress& key) { return live::handleShortcut(model_, key); }

void StatusBar::resized() {
    auto* top = getTopLevelComponent();
    compact_ = top ? theme::isCompact(top->getWidth(), top->getHeight()) : false;
    // Scale the 800 px widths to whatever width the bar has; the last chip absorbs rounding.
    const int w = getWidth();
    int x = 0;
    for (int i = 0; i < kChipCount; ++i) {
        const int cw = (i == kChipCount - 1) ? (w - x) : (kWidths[i] * w) / 800;
        chips_[static_cast<std::size_t>(i)].setBounds(x, 0, cw, getHeight());
        x += cw;
    }
}

void StatusBar::uiSnapshotChanged(const UiSnapshot& s) {
    const status::Chip chips[kChipCount] = {
        status::showChip(s), status::audioChip(s), status::modeChip(s), status::tempoChip(s), status::keyChip(s),
        status::midiChip(s), status::lightingChip(s), status::recordChip(s), status::broadcastChip(s), status::perfChip(s)};
    for (int i = 0; i < kChipCount; ++i) chips_[static_cast<std::size_t>(i)].setChip(chips[i], compact_);
    announceTransitions(s);
}

// Status changes are announced with the same words as the chips (APP-UI-09, app-ui 8).
void StatusBar::announceTransitions(const UiSnapshot& s) {
    auto say = [](const char* text) {
        juce::AccessibilityHandler::postAnnouncement(juce::String(text), juce::AccessibilityHandler::AnnouncementPriority::high);
    };
    const int lighting = static_cast<int>(s.lighting.state);
    if (lastLighting_ >= 0 && lighting != lastLighting_) {
        if (s.lighting.state == LightingUiState::Armed) say("Lighting armed. Lights are live.");
        else if (s.lighting.state == LightingUiState::Fault) say("Lighting fault. Arm again to resume.");
        else say("Lighting disarmed.");
    }
    lastLighting_ = lighting;

    const int rec = static_cast<int>(s.record.state);
    if (lastRecord_ >= 0 && rec != lastRecord_) {
        if (s.record.state == RecordUiState::Recording) say("Recording started.");
        else if (s.record.state == RecordUiState::Failed) say("Recording failed.");
    }
    lastRecord_ = rec;

    const int bc = static_cast<int>(s.broadcast.state);
    if (lastBroadcast_ >= 0 && bc != lastBroadcast_) {
        if (s.broadcast.state == BroadcastUiState::Live) say("Broadcast live.");
        else if (s.broadcast.state == BroadcastUiState::Failed) say("Broadcast error.");
        else if (s.broadcast.state == BroadcastUiState::Off) say("Broadcast off.");
    }
    lastBroadcast_ = bc;

    const int audio = static_cast<int>(s.audio.deviceState);
    if (lastAudio_ >= 0 && audio != lastAudio_) {
        if (s.audio.deviceState == AdapterState::Running) say("Audio device found.");
        else if (s.audio.deviceState == AdapterState::Fault || s.audio.deviceState == AdapterState::Degraded) say("Audio device lost.");
    }
    lastAudio_ = audio;
}

}  // namespace ss::app
