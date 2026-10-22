// StageSignal palette: keyboard map (spec section 11, REQ-X02, REQ-D11).
// Portable logic only: the JUCE layer translates key events into KeyChord and calls KeyMap::dispatch.
// Keyboard shortcuts are focused-window shortcuts, never global OS hotkeys.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/types.hpp"
#include "ss/palette/registry.hpp"

namespace ss::palette {

enum class Key : std::uint16_t {
    None = 0,
    D0 = '0', D1, D2, D3, D4, D5, D6, D7, D8, D9,
    A = 'A', B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    F1 = 0x101, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Space = 0x120, Enter, Escape, Tab, Backspace, Delete, Insert,
    Left, Right, Up, Down, Home, End, PageUp, PageDown,
    Comma, Period, Slash, Semicolon, Quote, LBracket, RBracket, Minus, Equals, Backslash, Backquote
};

enum Mod : std::uint8_t { kModNone = 0, kModCtrl = 1, kModShift = 2, kModAlt = 4 };

struct KeyChord {
    Key key = Key::None;
    std::uint8_t mods = kModNone;
    bool operator==(const KeyChord& o) const { return key == o.key && mods == o.mods; }
    bool operator!=(const KeyChord& o) const { return !(*this == o); }
    bool operator<(const KeyChord& o) const {
        return key != o.key ? static_cast<std::uint16_t>(key) < static_cast<std::uint16_t>(o.key) : mods < o.mods;
    }
};

std::string chordString(const KeyChord& c);                    // "Ctrl+Shift+F8", "1", "Ctrl+K"
std::optional<KeyChord> parseChord(const std::string& text);// case-insensitive, any modifier order
bool isPrintableKey(Key k);                                 // letters, digits, space, punctuation

// REQ-X02 / spec section 11: "never trigger scene pads while a text field is typing".
// With a text field focused a chord is withheld when it would type, edit or navigate text: printable keys (with Shift
// at most), Space/Enter/Tab/Backspace/Delete/arrows/Home/End/Page keys, and the clipboard/undo chords the field owns
// (Ctrl+A/C/V/X/Z/Y). Function keys, Ctrl+S, Ctrl+K and other non-text chords still dispatch, so Emergency blackout
// (F8 by default) stays reachable. AltGr (Ctrl+Alt+printable) counts as typing.
bool shouldDispatch(const KeyChord& chord, bool textFieldFocused);

enum class UiCommand : std::uint8_t { None, ShowKeyboardHelp };

struct Binding {
    KeyChord chord;
    ActionId action = ActionId::None;     // None when `ui` is set
    UiCommand ui = UiCommand::None;
    double value = 0.0;                   // carried in the Action (scene slot for pads)
};

struct DispatchResult {
    enum class Kind : std::uint8_t { Ignored, Action, Ui } kind = Kind::Ignored;
    Action action;                        // Kind::Action; source = Keyboard
    UiCommand ui = UiCommand::None;       // Kind::Ui
    const char* ignoredBecause = "";      // "unbound" or "text field has focus"
};

class KeyMap {
public:
    static KeyMap defaults();                               // F1, T, F5-F8, Shift+F8, Ctrl+S/K/Z/Y, 1-8 pads
    const std::vector<Binding>& bindings() const { return bindings_; }
    const Binding* lookup(const KeyChord& c) const;
    std::vector<Binding> bindingsFor(ActionId a) const;     // all chords of an action, ordered by chord

    // Adds a binding. A chord already bound to something else is a conflict: error unless replaceConflict.
    // Safety-critical actions may only use chords that still dispatch while a text field has focus.
    Status bind(const Binding& b, bool replaceConflict = false);
    bool unbind(const KeyChord& c);
    // Moves the binding on `from` to `to`.
    Status rebind(const KeyChord& from, const KeyChord& to, bool replaceConflict = false);
    // The binding that would conflict with binding `chord`, if any.
    const Binding* conflictFor(const KeyChord& chord) const { return lookup(chord); }
    void resetToDefaults() { *this = defaults(); }

    DispatchResult dispatch(const KeyChord& chord, bool textFieldFocused) const;

    // Persistence of user remaps as text lines "<chord>=<action id>:<value>" | "<chord>=help" | "<chord>=-" (removed).
    // Only differences from the defaults are exported.
    std::vector<std::string> exportOverrides() const;
    Status importOverrides(const std::vector<std::string>& lines);   // all-or-nothing, applied over the defaults

    // Imported bindings are checked as a set: the first claimant of a chord wins and the rest are reported.
    static KeyMap fromBindings(const std::vector<Binding>& list, std::vector<Binding>* rejected = nullptr);

private:
    std::vector<Binding> bindings_;
};

}  // namespace ss::palette
