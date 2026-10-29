// Opens and closes audience windows, one per audience output, and repaints them from the snapshot listener.
// Message thread only. Owner: ui-live. Setup > Displays drives it (SetupPanel owns the instance).
#pragma once
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModel.h"
#include "UI/Audience/AudienceWindow.h"

namespace ss::app::audience {

class OutputManager final : public UiModelListener, private juce::Timer {
public:
    explicit OutputManager(UiModel& model);
    ~OutputManager() override;

    // Opens the output full screen on the display with this stable id. Refuses when the display is the one that
    // holds the operator window (an audience window never lands on the operator display) and when it is already open.
    bool open(OutputId id, const juce::Displays::Display& display, juce::Component* operatorComponent, juce::String& reason);
    void close(OutputId id);
    void closeAll();
    bool isOpen(OutputId id) const;
    int openCount() const { return static_cast<int>(windows_.size()); }
    const juce::String& safetyNotice() const { return safetyNotice_; }

    void uiSnapshotChanged(const UiSnapshot& s) override;

private:
    void timerCallback() override;
    void validateWindows();

    struct Entry {
        OutputId id;
        std::unique_ptr<AudienceWindow> window;
        juce::Displays::Display display;
        juce::Array<juce::Displays::Display> topology;
        juce::Component::SafePointer<juce::Component> operatorComponent;
    };
    UiModel& model_;
    std::vector<Entry> windows_;
    juce::String safetyNotice_;
};

}  // namespace ss::app::audience
