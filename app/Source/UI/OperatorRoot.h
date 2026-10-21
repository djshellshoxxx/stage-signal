// Operator root component (app-ui 2): StatusBar (48) + TabHost + PerformanceStrip (60).
// Owner: shell-integration. Used by BOTH the standalone OperatorWindow and the CLAP editor.
// Owns the 30 Hz UiModelPoller, so no other component creates engine-state timers.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModelPoller.h"
#include "UI/PerformanceStrip.h"
#include "UI/StatusBar.h"
#include "UI/TabHost.h"

namespace ss::app {

class OperatorRoot final : public juce::Component {
public:
    explicit OperatorRoot(UiModel& model);
    ~OperatorRoot() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    TabHost& tabs() { return tabs_; }
    bool compact() const { return compact_; }
private:
    UiModel& model_;
    StatusBar status_;
    TabHost tabs_;
    PerformanceStrip strip_;
    UiModelPoller poller_;
    bool compact_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OperatorRoot)
};

}  // namespace ss::app
