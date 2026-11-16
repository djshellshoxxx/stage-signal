// Command Palette overlay (app-ui 12). Owner: ui-other. Open with Ctrl+K (wired by the window owner through open()).
// Search and ranking live in core/palette; this component lists results and submits them with ActionSource::Ui.
// StartRecording, StartBroadcast and ArmLighting never run in one keystroke: they open an inline confirmation.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModel.h"
#include "ss/palette/command_palette.hpp"
#include "ss/palette/keymap.hpp"

namespace ss::app {

class CommandPaletteOverlay final : public juce::Component,
                                    private UiModelListener,
                                    private juce::ListBoxModel,
                                    private juce::KeyListener {
public:
    explicit CommandPaletteOverlay(UiModel& model);
    ~CommandPaletteOverlay() override;

    // Shows the palette over `host` (the window root). The caller owns this object and keeps it alive.
    void open(juce::Component& host);
    // Hides it, removes it from its host and calls onClosed. Closing never runs an action.
    void close();
    bool isOpen() const { return isVisible(); }
    std::function<void()> onClosed;

    // The three actions that need an explicit second press from the palette.
    static bool needsConfirmation(ss::ActionId id);

    // Result rows for the current query (exposed for the self-tests).
    const std::vector<palette::SearchResult>& results() const { return results_; }
    void setQuery(const std::string& text);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    static constexpr int kPanelW = 560, kTopOffset = 72, kRowH = 26, kVisibleRows = 8;
    juce::Rectangle<int> panelBounds() const;

    // UiModelListener: the outcome of a palette submit closes the palette or shows the sanitised reason.
    void uiActionOutcome(const ActionOutcome& o) override;
    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    // KeyListener: arrows move the selection, Enter runs, Escape closes.
    bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override;

    void refresh();
    void moveSelection(int delta);
    void runRow(int row);
    void showArgument(const palette::SearchResult& r);
    void showConfirm(const palette::SearchResult& r);
    void hideInlineRows();
    bool submit(ss::ActionId id, const std::string& valueText);
    void commitArgument();
    void setStatus(const juce::String& text);

    UiModel& model_;
    palette::KeyMap keymap_;
    palette::CommandPalette palette_;
    std::vector<palette::SearchResult> results_;
    ss::ActionId pending_ = ss::ActionId::None;
    std::uint64_t pendingCorrelation_ = 0;

    juce::TextEditor query_;
    juce::ListBox list_;
    juce::Label status_, argLabel_, confirmLabel_;
    juce::TextEditor argValue_;
    juce::TextButton argRun_{"Run"}, argCancel_{"Cancel"}, confirmYes_{"Confirm"}, confirmNo_{"Cancel"};
    bool showingArg_ = false, showingConfirm_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CommandPaletteOverlay)
};

}  // namespace ss::app
