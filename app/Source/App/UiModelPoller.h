// Contract: the 30 Hz message-thread timer that drives UiModel::poll() (app-shell 4.1, UI row). Owner: shell-integration.
// OperatorRoot owns one. Panels do NOT create their own timers for engine state; they implement
// UiModelListener (or read model.snapshot() inside paint) and let this poller tick them.
#pragma once
#include <juce_events/juce_events.h>
#include "App/UiModel.h"

namespace ss::app {

class UiModelPoller final : private juce::Timer {
public:
    explicit UiModelPoller(UiModel& m, int hz = 30) : model_(m) { startTimerHz(hz); }
    ~UiModelPoller() override { stopTimer(); }
private:
    void timerCallback() override { model_.poll(); }
    UiModel& model_;
};

}  // namespace ss::app
