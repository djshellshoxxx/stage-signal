// Show Report export panel (app-ui 14). Owner: ui-other. Inline panel, not a dialog. The report logic (HTML and CSV,
// formula-injection guard, deterministic bytes) lives in core/diag; this view only collects the choices and posts
// AppCommandKind::ExportShowReport with this encoding (the handler owner must read it the same way):
//   text  = folder path            text2 = "from|to" when range is custom, otherwise empty
//   a     = include bitmask: 1 setlist, 2 timeline, 4 markers, 8 device events, 16 recording and broadcast
//   b     = format: 1 HTML, 2 CSV, 3 both
//   value = range: 1 this session, 2 since last start, 3 custom
#pragma once
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModel.h"
#include "UI/Overlays/FolderPicker.h"

namespace ss::app {

class ShowReportView final : public juce::Component, private UiModelListener {
public:
    explicit ShowReportView(UiModel& model);
    ~ShowReportView() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

    // Validates the choices and posts the command. Returns the operator-facing line (also shown in the panel).
    std::string exportReport();
    // The statement shown next to the preview. It never claims a count the view does not have.
    static const char* privacyStatement();
    // Names of the checked items, for the preview line.
    std::string includedNames() const;

    std::uint32_t includeMask() const;
    int formatCode() const;
    int rangeCode() const;

private:
    void uiCommandOutcome(const CommandOutcome& o) override;

    UiModel& model_;
    FolderPicker folderPicker_;
    std::string folderPath_;
    std::uint64_t pendingCorrelation_ = 0;

    juce::Label title_, rangeLabel_, fromLabel_, toLabel_, formatLabel_, folderLabel_, previewText_, status_;
    juce::ComboBox range_, format_;
    juce::TextEditor from_, to_;
    juce::ToggleButton setlist_{"Setlist of scene and cue changes"}, timeline_{"Energy, BPM and key timeline"},
        markers_{"Markers"}, devices_{"Device events"}, recording_{"Recording and broadcast changes"};
    juce::TextButton chooseFolder_{"Choose folder"}, preview_{"Preview what is included"}, export_{"Export"};
    juce::TextButton cancel_{"Close"};
public:
    std::function<void()> onClosed;   // the owner hides the panel

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ShowReportView)
};

}  // namespace ss::app
