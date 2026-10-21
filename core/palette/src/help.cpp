#include "ss/palette/help.hpp"
#include <algorithm>

namespace ss::palette {

namespace {
bool isPlainDigit(const KeyChord& c) { return c.mods == kModNone && c.key >= Key::D0 && c.key <= Key::D9; }
std::string pad(const std::string& s, std::size_t w) { return s.size() >= w ? s + " " : s + std::string(w - s.size(), ' '); }
}  // namespace

std::string shortcutText(const KeyMap& map, ActionId action) {
    const auto bs = map.bindingsFor(action);
    if (bs.empty()) return "";
    // collapse a run of plain digits ("1".."8") into "1-8"
    std::vector<int> digits;
    std::vector<std::string> others;
    for (const auto& b : bs) {
        if (isPlainDigit(b.chord)) digits.push_back(static_cast<int>(b.chord.key) - '0');
        else others.push_back(chordString(b.chord));
    }
    std::string out;
    std::sort(digits.begin(), digits.end());
    for (std::size_t i = 0; i < digits.size();) {
        std::size_t j = i;
        while (j + 1 < digits.size() && digits[j + 1] == digits[j] + 1) ++j;
        if (!out.empty()) out += ", ";
        out += std::to_string(digits[i]);
        if (j > i + 1) out += "-" + std::to_string(digits[j]);
        else if (j == i + 1) out += ", " + std::to_string(digits[j]);
        i = j + 1;
    }
    for (const auto& o : others) { if (!out.empty()) out += ", "; out += o; }
    return out;
}

std::vector<HelpRow> keyboardHelpRows(const KeyMap& map) {
    std::vector<HelpRow> rows;
    for (const auto& info : Registry::all()) {
        const std::string keys = shortcutText(map, info.id);
        if (keys.empty()) continue;
        HelpRow r;
        r.keys = keys; r.action = info.name; r.description = info.description; r.category = info.category;
        r.safetyCritical = info.safetyCritical; r.takesValue = info.value.takesValue;
        rows.push_back(std::move(r));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const HelpRow& a, const HelpRow& b) {
        return static_cast<int>(a.category) < static_cast<int>(b.category);
    });
    return rows;
}

std::string keyboardHelpText(const KeyMap& map) {
    std::string out = "StageSignal keyboard shortcuts\n\n";
    // F1 and other UI commands
    std::vector<std::string> helpKeys;
    for (const auto& b : map.bindings()) if (b.ui == UiCommand::ShowKeyboardHelp) helpKeys.push_back(chordString(b.chord));
    std::sort(helpKeys.begin(), helpKeys.end());
    std::string hk;
    for (const auto& k : helpKeys) { if (!hk.empty()) hk += ", "; hk += k; }

    const auto rows = keyboardHelpRows(map);
    std::size_t width = hk.size();
    for (const auto& r : rows) width = std::max(width, r.keys.size());
    width += 2;

    if (!hk.empty()) out += "Help\n  " + pad(hk, width) + "Keyboard shortcuts (this sheet)\n\n";
    int lastCat = -1;
    for (const auto& r : rows) {
        if (static_cast<int>(r.category) != lastCat) {
            if (lastCat != -1) out += "\n";
            out += std::string(categoryName(r.category)) + "\n";
            lastCat = static_cast<int>(r.category);
        }
        out += "  " + pad(r.keys, width) + r.action;
        if (r.takesValue) out += " (takes a value)";
        if (r.safetyCritical) out += " [SAFETY]";
        out += "\n";
    }

    std::vector<std::string> paletteOnly;
    for (const auto& info : Registry::all()) if (map.bindingsFor(info.id).empty()) paletteOnly.push_back(info.name);
    std::string palKey = shortcutText(map, ActionId::OpenCommandPalette);
    if (palKey.empty()) palKey = "unbound; use the Edit and tools menu";

    out += "\nWhile typing\n";
    out += "  Letters, digits, space and editing keys go to the text field, so scene pads never fire while you type.\n";
    out += "  Function keys (including the blackout key) and chords such as Ctrl+S keep working.\n";
    out += "\nGood to know\n";
    out += "  Shortcuts work while the StageSignal window has focus. They are not global system hotkeys.\n";
    out += "  Blackout is latched: restore it explicitly.\n";
    out += "\nCommand Palette (" + palKey + ")\n";
    out += "  Type part of an action name to run any action, including these without a shortcut:\n";
    out += "  " + std::to_string(paletteOnly.size()) + " actions: ";
    for (std::size_t i = 0; i < paletteOnly.size(); ++i) { if (i) out += ", "; out += paletteOnly[i]; }
    out += "\n";
    return out;
}

}  // namespace ss::palette
