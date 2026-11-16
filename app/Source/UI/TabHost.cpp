#include "UI/TabHost.h"
#include "UI/Theme.h"

namespace ss::app {

TabHost::TabHost(UiModel& model) : juce::TabbedComponent(juce::TabbedButtonBar::TabsAtTop) {
    live_ = std::make_unique<LivePanel>(model);
    setup_ = std::make_unique<SetupPanel>(model);
    mediaText_ = std::make_unique<MediaTextPanel>(model);
    midiLighting_ = std::make_unique<MidiLightingPanel>(model);
    recordBroadcast_ = std::make_unique<RecordBroadcastPanel>(model);
    diagnostics_ = std::make_unique<DiagnosticsPanel>(model);
    help_ = std::make_unique<HelpPanel>(model);
    setTabBarDepth(theme::kTabBarH);
    setOutline(0);
    const auto bg = theme::bgPanel();
    addTab("Live", bg, live_.get(), false);
    addTab("Setup", bg, setup_.get(), false);
    addTab("Media / Text", bg, mediaText_.get(), false);
    addTab("MIDI / Lighting", bg, midiLighting_.get(), false);
    addTab("Record / Broadcast", bg, recordBroadcast_.get(), false);
    addTab("Diagnostics", bg, diagnostics_.get(), false);
    addTab("Help", bg, help_.get(), false);
}

TabHost::~TabHost() { clearTabs(); }

}  // namespace ss::app
