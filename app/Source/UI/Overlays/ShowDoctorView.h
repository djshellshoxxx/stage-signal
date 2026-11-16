// Show Doctor view (app-ui 10). Owner: ui-other. The check is the pure core/doctor function; this component
// builds its input from UiSnapshot, renders the findings and sends only the safe one-click fixes.
// Running the check transmits nothing, opens no recorder or broadcaster, and starts no camera.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModel.h"
#include "ss/doctor/doctor.hpp"

namespace ss::app {

class ShowDoctorView final : public juce::Component {
public:
    explicit ShowDoctorView(UiModel& model);
    ~ShowDoctorView() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Call from the owner's uiSnapshotChanged on every snapshot. Cheap: only tracks how long audio has been quiet.
    void observe(const UiSnapshot& s);
    // Runs the check now (operator button, or the first time a snapshot arrives).
    void runCheck(const UiSnapshot& s);
    bool hasRun() const { return hasRun_; }

    const std::vector<doctor::Finding>& findings() const { return findings_; }
    // Maps the snapshot to the doctor's plain-data input. Exposed for the self-tests.
    doctor::EnvironmentSnapshot environmentFrom(const UiSnapshot& s) const;
    // "Ready", "Ready with 3 warnings", "Not ready: 1 blocker".
    static std::string bannerFor(const std::vector<doctor::Finding>& f);

private:
    class Row;
    void rebuildRows();
    void applyFix(std::size_t index);
    void layoutRows();

    UiModel& model_;
    ss::Ns silentSinceNs_ = -1;          // snapshot time when the input went quiet, -1 while signal is alive
    std::vector<doctor::Finding> findings_;
    bool hasRun_ = false;
    juce::Label banner_, status_;
    juce::TextButton runButton_{"Run check"};
    juce::Viewport viewport_;
    juce::Component body_;
    std::vector<std::unique_ptr<Row>> rows_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ShowDoctorView)
};

}  // namespace ss::app
