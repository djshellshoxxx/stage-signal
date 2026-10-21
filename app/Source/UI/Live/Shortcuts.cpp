#include "UI/Live/Shortcuts.h"
#include "UI/Live/Actions.h"
#include "ss/palette/keymap.hpp"

namespace ss::app::live {
namespace {

bool toChord(const juce::KeyPress& k, palette::KeyChord& out) {
    const int code = k.getKeyCode();
    palette::Key key = palette::Key::None;
    if (code >= 'a' && code <= 'z') key = static_cast<palette::Key>(code - 'a' + 'A');
    else if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) key = static_cast<palette::Key>(code);
    else if (code >= juce::KeyPress::F1Key && code <= juce::KeyPress::F1Key + 11)
        key = static_cast<palette::Key>(static_cast<int>(palette::Key::F1) + (code - juce::KeyPress::F1Key));
    else if (code == juce::KeyPress::escapeKey) key = palette::Key::Escape;
    else if (code == juce::KeyPress::tabKey) key = palette::Key::Tab;
    else if (code == juce::KeyPress::spaceKey) key = palette::Key::Space;
    else if (code == juce::KeyPress::returnKey) key = palette::Key::Enter;
    else if (code == juce::KeyPress::leftKey) key = palette::Key::Left;
    else if (code == juce::KeyPress::rightKey) key = palette::Key::Right;
    else if (code == juce::KeyPress::upKey) key = palette::Key::Up;
    else if (code == juce::KeyPress::downKey) key = palette::Key::Down;
    else return false;

    std::uint8_t mods = palette::kModNone;
    const auto m = k.getModifiers();
    if (m.isCtrlDown()) mods |= palette::kModCtrl;
    if (m.isShiftDown()) mods |= palette::kModShift;
    if (m.isAltDown()) mods |= palette::kModAlt;
    out.key = key;
    out.mods = mods;
    return true;
}

const palette::KeyMap& keymap() {
    static const palette::KeyMap map = palette::KeyMap::defaults();
    return map;
}

}  // namespace

bool handleShortcut(UiModel& model, const juce::KeyPress& key) {
    palette::KeyChord chord;
    if (!toChord(key, chord)) return false;
    const bool textFocused = uikit::textFieldFocused();
    const auto r = keymap().dispatch(chord, textFocused);
    if (r.kind == palette::DispatchResult::Kind::Action) {
        model.submitAction(r.action);
        return true;
    }
    // app-ui 6 binds T to TapTempo, but core/palette KeyMap::defaults() does not carry it yet. Until it does,
    // the Live tab adds the binding here. Inert while a text field has focus, like the other letter keys.
    if (!textFocused && chord.key == palette::Key::T && chord.mods == palette::kModNone) {
        uikit::sendAction(model, ss::ActionId::TapTempo);
        return true;
    }
    return false;
}

}  // namespace ss::app::live
