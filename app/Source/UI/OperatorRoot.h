// Operator root component (app-ui 2): StatusBar (48) + TabHost + PerformanceStrip (60).
// Owner: shell-integration. Used by BOTH the standalone OperatorWindow and the CLAP editor.
// Owns the 30 Hz UiModelPoller, so no other component creates engine-state timers.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModelPoller.h"
#include "UI/Overlays/CommandPalette.h"
#include "UI/PerformanceStrip.h"
#include "UI/StatusBar.h"
#include "UI/TabHost.h"

namespace ss::app {

class OperatorRoot final : public juce::Component, private UiModelListener {
public:
    explicit OperatorRoot(UiModel& model);
    ~OperatorRoot() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    TabHost& tabs() { return tabs_; }
    bool compact() const { return compact_; }
    bool keyPressed(const juce::KeyPress& key) override;    // keys no focused control used: Ctrl/Cmd+K, then the shortcut map
    CommandPaletteOverlay& palette() { return palette_; }
    bool bannerVisible() const { return banner_.isVisible(); }
private:
    void uiSnapshotChanged(const UiSnapshot& s) override;
    void sendRecovery(AppCommandKind k);
    UiModel& model_;
    StatusBar status_;
    TabHost tabs_;
    PerformanceStrip strip_;
    UiModelPoller poller_;
    CommandPaletteOverlay palette_;
    juce::Label banner_;
    juce::TextButton restoreAuto_{"Restore autosave"}, openLast_{"Open last saved"}, startEmpty_{"Start empty"};
    bool bannerWanted_ = false, recoveryButtons_ = false;
    bool compact_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OperatorRoot)
};

}  // namespace ss::app
