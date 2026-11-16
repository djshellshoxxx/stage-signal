#include "ss/palette/keymap.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

namespace ss::palette {

namespace {

struct KeyName { Key key; const char* name; };
const KeyName kNames[] = {
    {Key::Space, "Space"}, {Key::Enter, "Enter"}, {Key::Escape, "Esc"}, {Key::Tab, "Tab"}, {Key::Backspace, "Backspace"},
    {Key::Delete, "Delete"}, {Key::Insert, "Insert"}, {Key::Left, "Left"}, {Key::Right, "Right"}, {Key::Up, "Up"},
    {Key::Down, "Down"}, {Key::Home, "Home"}, {Key::End, "End"}, {Key::PageUp, "PageUp"}, {Key::PageDown, "PageDown"},
    {Key::Comma, ","}, {Key::Period, "."}, {Key::Slash, "/"}, {Key::Semicolon, ";"}, {Key::Quote, "'"},
    {Key::LBracket, "["}, {Key::RBracket, "]"}, {Key::Minus, "-"}, {Key::Equals, "="}, {Key::Backslash, "\\"},
    {Key::Backquote, "`"},
};

std::string keyName(Key k) {
    const auto v = static_cast<std::uint16_t>(k);
    if ((v >= '0' && v <= '9') || (v >= 'A' && v <= 'Z')) return std::string(1, static_cast<char>(v));
    if (v >= static_cast<std::uint16_t>(Key::F1) && v <= static_cast<std::uint16_t>(Key::F12))
        return "F" + std::to_string(v - static_cast<std::uint16_t>(Key::F1) + 1);
    for (const auto& n : kNames) if (n.key == k) return n.name;
    return "";
}

std::string upperAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool isFunctionKey(Key k) {
    const auto v = static_cast<std::uint16_t>(k);
    return v >= static_cast<std::uint16_t>(Key::F1) && v <= static_cast<std::uint16_t>(Key::F12);
}

bool isEditNavKey(Key k) {
    switch (k) {
        case Key::Space: case Key::Enter: case Key::Tab: case Key::Backspace: case Key::Delete: case Key::Insert:
        case Key::Left: case Key::Right: case Key::Up: case Key::Down: case Key::Home: case Key::End:
        case Key::PageUp: case Key::PageDown: return true;
        default: return false;
    }
}

bool isTextOwnedCtrl(Key k) {
    return k == Key::A || k == Key::C || k == Key::V || k == Key::X || k == Key::Z || k == Key::Y;
}

Binding mk(Key k, std::uint8_t mods, ActionId a, double value = 0.0) {
    Binding b; b.chord = {k, mods}; b.action = a; b.value = value; return b;
}

}  // namespace

std::string chordString(const KeyChord& c) {
    std::string s;
    if (c.mods & kModCtrl) s += "Ctrl+";
    if (c.mods & kModShift) s += "Shift+";
    if (c.mods & kModAlt) s += "Alt+";
    return s + keyName(c.key);
}

std::optional<KeyChord> parseChord(const std::string& text) {
    KeyChord c;
    std::string cur;
    std::vector<std::string> parts;
    for (std::size_t i = 0; i < text.size(); ++i) {
        // '+' separates parts, except when it is the key itself at the end ("Ctrl++" is not supported: use Equals)
        if (text[i] == '+') { parts.push_back(cur); cur.clear(); }
        else cur += text[i];
    }
    parts.push_back(cur);
    if (parts.empty()) return std::nullopt;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string m = upperAscii(parts[i]);
        if (m == "CTRL" || m == "CONTROL") c.mods |= kModCtrl;
        else if (m == "SHIFT") c.mods |= kModShift;
        else if (m == "ALT") c.mods |= kModAlt;
        else return std::nullopt;
    }
    const std::string k = upperAscii(parts.back());
    if (k.empty()) return std::nullopt;
    if (k.size() == 1 && ((k[0] >= '0' && k[0] <= '9') || (k[0] >= 'A' && k[0] <= 'Z'))) {
        c.key = static_cast<Key>(k[0]);
        return c;
    }
    if (k.size() >= 2 && k[0] == 'F') {
        char* end = nullptr;
        const long n = std::strtol(k.c_str() + 1, &end, 10);
        if (*end == '\0' && n >= 1 && n <= 12) { c.key = static_cast<Key>(static_cast<std::uint16_t>(Key::F1) + (n - 1)); return c; }
    }
    for (const auto& n : kNames) {
        if (k == upperAscii(n.name) || (k == "ESCAPE" && n.key == Key::Escape)) { c.key = n.key; return c; }
    }
    return std::nullopt;
}

bool isPrintableKey(Key k) {
    const auto v = static_cast<std::uint16_t>(k);
    if ((v >= '0' && v <= '9') || (v >= 'A' && v <= 'Z')) return true;
    switch (k) {
        case Key::Space: case Key::Comma: case Key::Period: case Key::Slash: case Key::Semicolon: case Key::Quote:
        case Key::LBracket: case Key::RBracket: case Key::Minus: case Key::Equals: case Key::Backslash:
        case Key::Backquote: return true;
        default: return false;
    }
}

bool shouldDispatch(const KeyChord& c, bool textFieldFocused) {
    if (c.key == Key::None) return false;
    if (!textFieldFocused) return true;
    if (isFunctionKey(c.key)) return true;                         // not text input: F8 blackout stays reachable
    const bool ctrl = (c.mods & kModCtrl) != 0, alt = (c.mods & kModAlt) != 0;
    if (ctrl && alt && isPrintableKey(c.key)) return false;        // AltGr produces characters
    if (isEditNavKey(c.key) && !alt) return false;                 // editing and caret movement belong to the field
    if (ctrl && isTextOwnedCtrl(c.key)) return false;              // clipboard, select all, text undo/redo
    if (!ctrl && !alt && isPrintableKey(c.key)) return false;      // typing (Shift only changes the character)
    return true;
}

KeyMap KeyMap::defaults() {
    KeyMap m;
    Binding help; help.chord = {Key::F1, kModNone}; help.ui = UiCommand::ShowKeyboardHelp;
    m.bindings_.push_back(help);
    m.bindings_.push_back(mk(Key::F5, kModNone, ActionId::NextScene));
    m.bindings_.push_back(mk(Key::F6, kModNone, ActionId::Pause));
    m.bindings_.push_back(mk(Key::F7, kModNone, ActionId::Resume));
    m.bindings_.push_back(mk(Key::F8, kModNone, ActionId::EmergencyBlackout));
    m.bindings_.push_back(mk(Key::F8, kModShift, ActionId::RestoreBlackout));
    m.bindings_.push_back(mk(Key::S, kModCtrl, ActionId::SaveShow));
    m.bindings_.push_back(mk(Key::K, kModCtrl, ActionId::OpenCommandPalette));
    m.bindings_.push_back(mk(Key::Z, kModCtrl, ActionId::Undo));
    m.bindings_.push_back(mk(Key::Y, kModCtrl, ActionId::Redo));
    m.bindings_.push_back(mk(Key::T, kModNone, ActionId::TapTempo));
    for (int i = 0; i < 8; ++i)
        m.bindings_.push_back(mk(static_cast<Key>('1' + i), kModNone, ActionId::SceneSelect, static_cast<double>(i)));
    return m;
}

const Binding* KeyMap::lookup(const KeyChord& c) const {
    for (const auto& b : bindings_) if (b.chord == c) return &b;
    return nullptr;
}

std::vector<Binding> KeyMap::bindingsFor(ActionId a) const {
    std::vector<Binding> v;
    for (const auto& b : bindings_) if (b.action == a && b.ui == UiCommand::None) v.push_back(b);
    std::sort(v.begin(), v.end(), [](const Binding& x, const Binding& y) { return x.chord < y.chord; });
    return v;
}

Status KeyMap::bind(const Binding& b, bool replaceConflict) {
    if (b.chord.key == Key::None || keyName(b.chord.key).empty())
        return makeError(ErrorCategory::InvalidArgument, "no key");
    if ((b.action == ActionId::None) == (b.ui == UiCommand::None))
        return makeError(ErrorCategory::InvalidArgument, "binding needs exactly one of action or UI command");
    if (b.action != ActionId::None) {
        const ActionInfo* info = Registry::find(b.action);
        if (!info) return makeError(ErrorCategory::InvalidArgument, "unknown action");
        if (info->safetyCritical && !shouldDispatch(b.chord, true))
            return makeError(ErrorCategory::InvalidArgument, "safety-critical actions must stay reachable while typing");
        if (!info->value.takesValue && b.value != 0.0)
            return makeError(ErrorCategory::InvalidArgument, "action takes no value");
        if (info->value.takesValue && (b.value < info->value.min || b.value > info->value.max))
            return makeError(ErrorCategory::InvalidArgument, "value out of range");
    }
    for (auto it = bindings_.begin(); it != bindings_.end(); ++it) {
        if (it->chord != b.chord) continue;
        if (!replaceConflict) {
            std::string what = it->ui != UiCommand::None ? "keyboard help" : Registry::find(it->action) ? Registry::find(it->action)->name : "action";
            return makeError(ErrorCategory::InvalidArgument, chordString(b.chord) + " is already bound to " + what);
        }
        bindings_.erase(it);
        break;
    }
    bindings_.push_back(b);
    return okStatus();
}

bool KeyMap::unbind(const KeyChord& c) {
    auto it = std::find_if(bindings_.begin(), bindings_.end(), [&](const Binding& b) { return b.chord == c; });
    if (it == bindings_.end()) return false;
    bindings_.erase(it);
    return true;
}

Status KeyMap::rebind(const KeyChord& from, const KeyChord& to, bool replaceConflict) {
    const Binding* cur = lookup(from);
    if (!cur) return makeError(ErrorCategory::InvalidArgument, "nothing bound to " + chordString(from));
    if (from == to) return okStatus();
    Binding moved = *cur;
    moved.chord = to;
    KeyMap trial = *this;                                    // all-or-nothing
    trial.unbind(from);
    if (auto s = trial.bind(moved, replaceConflict); !s.ok()) return s;
    *this = std::move(trial);
    return okStatus();
}

DispatchResult KeyMap::dispatch(const KeyChord& chord, bool textFieldFocused) const {
    DispatchResult r;
    const Binding* b = lookup(chord);
    if (!b) { r.ignoredBecause = "unbound"; return r; }
    if (!shouldDispatch(chord, textFieldFocused)) { r.ignoredBecause = "text field has focus"; return r; }
    if (b->ui != UiCommand::None) { r.kind = DispatchResult::Kind::Ui; r.ui = b->ui; return r; }
    r.kind = DispatchResult::Kind::Action;
    r.action.actionId = id(b->action);
    r.action.source = ActionSource::Keyboard;
    r.action.value = b->value;
    return r;
}

KeyMap KeyMap::fromBindings(const std::vector<Binding>& list, std::vector<Binding>* rejected) {
    KeyMap m;
    for (const auto& b : list) {
        if (!m.bind(b, false).ok() && rejected) rejected->push_back(b);
    }
    return m;
}

namespace {
std::string encode(const Binding& b) {
    if (b.ui == UiCommand::ShowKeyboardHelp) return chordString(b.chord) + "=help";
    std::string v = std::to_string(b.value);
    return chordString(b.chord) + "=" + std::to_string(id(b.action)) + ":" + v;
}
}  // namespace

std::vector<std::string> KeyMap::exportOverrides() const {
    const KeyMap def = defaults();
    std::vector<std::string> out;
    std::vector<Binding> mine = bindings_;
    std::sort(mine.begin(), mine.end(), [](const Binding& a, const Binding& b) { return a.chord < b.chord; });
    for (const auto& b : mine) {
        const Binding* d = def.lookup(b.chord);
        if (!d || d->action != b.action || d->ui != b.ui || d->value != b.value) out.push_back(encode(b));
    }
    std::vector<Binding> dsorted = def.bindings_;
    std::sort(dsorted.begin(), dsorted.end(), [](const Binding& a, const Binding& b) { return a.chord < b.chord; });
    for (const auto& d : dsorted) if (!lookup(d.chord)) out.push_back(chordString(d.chord) + "=-");
    return out;
}

Status KeyMap::importOverrides(const std::vector<std::string>& lines) {
    KeyMap m = defaults();
    for (const auto& line : lines) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) return makeError(ErrorCategory::InvalidArgument, "bad key override line");
        auto chord = parseChord(line.substr(0, eq));
        if (!chord) return makeError(ErrorCategory::InvalidArgument, "bad key in override");
        const std::string rhs = line.substr(eq + 1);
        if (rhs == "-") { m.unbind(*chord); continue; }
        Binding b; b.chord = *chord;
        if (rhs == "help") b.ui = UiCommand::ShowKeyboardHelp;
        else {
            const auto colon = rhs.find(':');
            char* end = nullptr;
            const unsigned long aid = std::strtoul(rhs.c_str(), &end, 10);
            if (end == rhs.c_str() || colon == std::string::npos) return makeError(ErrorCategory::InvalidArgument, "bad action in override");
            b.action = static_cast<ActionId>(aid);
            b.value = std::strtod(rhs.c_str() + colon + 1, nullptr);
        }
        if (auto s = m.bind(b, true); !s.ok()) return s;
    }
    *this = std::move(m);
    return okStatus();
}

}  // namespace ss::palette
