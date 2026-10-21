// StageSignal palette: keyboard-help text generator, shared by the Help tab and the F1 sheet.
#pragma once
#include <string>
#include <vector>
#include "ss/palette/keymap.hpp"

namespace ss::palette {

struct HelpRow {
    std::string keys;            // "F5", "1-8", "Ctrl+K, Alt+P"
    std::string action;          // display name
    std::string description;
    Category category = Category::Scenes;
    bool safetyCritical = false; // rendered with a text label, never colour alone
    bool takesValue = false;
};

// Shortcut text for one action from the current keymap: consecutive unmodified digit pads collapse to "1-8";
// otherwise chords are joined with ", ". Empty when the action has no shortcut.
std::string shortcutText(const KeyMap& map, ActionId action);

// One row per action that has at least one binding, ordered by category then registry order. UI-only commands (F1)
// are listed separately by keyboardHelpText.
std::vector<HelpRow> keyboardHelpRows(const KeyMap& map);

// Plain-text sheet: grouped by category, safety-critical rows tagged "[SAFETY]", followed by the typing rule, the
// "not global hotkeys" note, the actions that only the palette reaches, and how to open the palette.
std::string keyboardHelpText(const KeyMap& map);

}  // namespace ss::palette
