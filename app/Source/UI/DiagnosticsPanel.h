// Diagnostics tab (app-ui 5.6): Show Doctor (10), live status, event journal viewer, support bundle, Show Report (14).
// Owner: ui-other. Pieces live in UI/Overlays/ and this file; reads UiSnapshot only.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "UI/Overlays/FolderPicker.h"
#include "UI/Overlays/ShowDoctorView.h"
#include "UI/Overlays/ShowReportView.h"
#include "UI/PanelBase.h"

namespace ss::app {

class DiagnosticsPanel final : public PanelBase {
public:
    explicit DiagnosticsPanel(UiModel& model);
    ~DiagnosticsPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class JournalModel;
    void uiSnapshotChanged(const UiSnapshot& s) override;
    void uiCommandOutcome(const CommandOutcome& o) override;
    void refreshStatus(const UiSnapshot& s);
    void refreshJournal(const UiSnapshot& s);

    ShowDoctorView doctor_;
    juce::TextEditor status_, filter_;
    juce::ListBox journal_;
    std::unique_ptr<JournalModel> journalModel_;
    juce::Label journalTitle_, bundleStatus_;
    juce::TextButton showReport_{"Show Report"}, supportBundle_{"Export support bundle"};
    ShowReportView report_;
    FolderPicker bundlePicker_;
    std::string statusText_;
    std::vector<DiagEvent> journalRows_;
    ss::Ns nowNs_ = 0;
    std::size_t journalKeyCount_ = 0;
    ss::Ns journalKeyNewest_ = -1;
    std::string journalKeyNeedle_;
    std::uint64_t bundleCorrelation_ = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DiagnosticsPanel)
};

}  // namespace ss::app
