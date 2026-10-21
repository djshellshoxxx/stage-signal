// Tab host: Live, Setup, MediaText, MidiLighting, RecordBroadcast, Diagnostics, Help (app-ui 5).
// Owner: shell-integration. Owns one panel per tab; ui-* workers own the panel classes, not this file.
// Uses juce::TabbedComponent for the skeleton; the stencil-styled custom tab bar is a ui-live follow-up that
// replaces only this class's internals (the panel accessors below stay).
#pragma once
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/DiagnosticsPanel.h"
#include "UI/HelpPanel.h"
#include "UI/LivePanel.h"
#include "UI/MediaTextPanel.h"
#include "UI/MidiLightingPanel.h"
#include "UI/RecordBroadcastPanel.h"
#include "UI/SetupPanel.h"

namespace ss::app {

enum class Tab : int { Live, Setup, MediaText, MidiLighting, RecordBroadcast, Diagnostics, Help, Count };

class TabHost final : public juce::TabbedComponent {
public:
    explicit TabHost(UiModel& model);
    ~TabHost() override;
    void showTab(Tab t) { setCurrentTabIndex(static_cast<int>(t)); }
    Tab currentTab() const { return static_cast<Tab>(getCurrentTabIndex()); }

    LivePanel& live() { return *live_; }
    SetupPanel& setup() { return *setup_; }
    MediaTextPanel& mediaText() { return *mediaText_; }
    MidiLightingPanel& midiLighting() { return *midiLighting_; }
    RecordBroadcastPanel& recordBroadcast() { return *recordBroadcast_; }
    DiagnosticsPanel& diagnostics() { return *diagnostics_; }
    HelpPanel& help() { return *help_; }

private:
    std::unique_ptr<LivePanel> live_;
    std::unique_ptr<SetupPanel> setup_;
    std::unique_ptr<MediaTextPanel> mediaText_;
    std::unique_ptr<MidiLightingPanel> midiLighting_;
    std::unique_ptr<RecordBroadcastPanel> recordBroadcast_;
    std::unique_ptr<DiagnosticsPanel> diagnostics_;
    std::unique_ptr<HelpPanel> help_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TabHost)
};

}  // namespace ss::app
