#include "doctest/doctest.h"
#include "ss/palette/keymap.hpp"

using namespace ss;
using namespace ss::palette;

namespace {
KeyChord C(const char* s) { auto c = parseChord(s); REQUIRE(c.has_value()); return *c; }
}

TEST_CASE("REQ-X02: default keyboard map matches the specification") {
    const KeyMap km = KeyMap::defaults();
    auto action = [&](const char* chord) { const Binding* b = km.lookup(C(chord)); REQUIRE(b); return b->action; };
    CHECK(km.lookup(C("F1"))->ui == UiCommand::ShowKeyboardHelp);
    CHECK(action("F5") == ActionId::NextScene);
    CHECK(action("F6") == ActionId::Pause);
    CHECK(action("F7") == ActionId::Resume);
    CHECK(action("F8") == ActionId::EmergencyBlackout);
    CHECK(action("Shift+F8") == ActionId::RestoreBlackout);
    CHECK(action("Ctrl+S") == ActionId::SaveShow);
    CHECK(action("Ctrl+K") == ActionId::OpenCommandPalette);
    CHECK(action("Ctrl+Z") == ActionId::Undo);
    CHECK(action("Ctrl+Y") == ActionId::Redo);
    for (int i = 1; i <= 8; ++i) {
        const Binding* b = km.lookup(C(std::to_string(i).c_str()));
        REQUIRE(b);
        CHECK(b->action == ActionId::SceneSelect);
        CHECK(b->value == i - 1);                       // pad 1 is slot 0
    }
    CHECK(km.lookup(C("9")) == nullptr);
    CHECK(km.bindings().size() == 18);
    // recording and broadcast have no keyboard toggles: explicit Start/Stop live in the UI
    CHECK(km.bindingsFor(ActionId::StartRecording).empty());
    CHECK(km.bindingsFor(ActionId::StopBroadcast).empty());
}

TEST_CASE("REQ-X02: chords format and parse symmetrically") {
    CHECK(chordString(C("ctrl+shift+f8")) == "Ctrl+Shift+F8");
    CHECK(chordString(C("Shift+Ctrl+K")) == "Ctrl+Shift+K");
    CHECK(chordString(C("esc")) == "Esc");
    CHECK(chordString(C("Alt+PageDown")) == "Alt+PageDown");
    CHECK_FALSE(parseChord("").has_value());
    CHECK_FALSE(parseChord("Ctrl+").has_value());
    CHECK_FALSE(parseChord("Hyper+K").has_value());
    CHECK_FALSE(parseChord("F13").has_value());
    CHECK_FALSE(parseChord("Ctrl+Nothing").has_value());
    const KeyMap def = KeyMap::defaults();
    for (const auto& b : def.bindings()) {
        auto back = parseChord(chordString(b.chord));
        REQUIRE(back.has_value());
        CHECK(*back == b.chord);
    }
}

TEST_CASE("REQ-X02: never trigger scene pads while typing") {
    // T-UI logic: focus in a text field
    for (char d = '1'; d <= '8'; ++d) {
        const KeyChord pad{static_cast<Key>(d), kModNone};
        CHECK(shouldDispatch(pad, false));
        CHECK_FALSE(shouldDispatch(pad, true));
    }
    const KeyMap km = KeyMap::defaults();
    auto r = km.dispatch(C("3"), true);
    CHECK(r.kind == DispatchResult::Kind::Ignored);
    CHECK(std::string(r.ignoredBecause) == "text field has focus");
    r = km.dispatch(C("3"), false);
    REQUIRE(r.kind == DispatchResult::Kind::Action);
    CHECK(r.action.actionId == id(ActionId::SceneSelect));
    CHECK(r.action.value == 2.0);
    CHECK(r.action.source == ActionSource::Keyboard);
}

TEST_CASE("REQ-X02: emergency blackout stays reachable while a text field has focus") {
    const KeyMap km = KeyMap::defaults();
    auto r = km.dispatch(C("F8"), true);
    REQUIRE(r.kind == DispatchResult::Kind::Action);
    CHECK(r.action.actionId == id(ActionId::EmergencyBlackout));
    r = km.dispatch(C("Shift+F8"), true);
    REQUIRE(r.kind == DispatchResult::Kind::Action);
    CHECK(r.action.actionId == id(ActionId::RestoreBlackout));
    // other non-text chords too
    CHECK(km.dispatch(C("F1"), true).kind == DispatchResult::Kind::Ui);
    CHECK(km.dispatch(C("Ctrl+S"), true).kind == DispatchResult::Kind::Action);
    CHECK(km.dispatch(C("Ctrl+K"), true).kind == DispatchResult::Kind::Action);
    CHECK(km.dispatch(C("F5"), true).kind == DispatchResult::Kind::Action);
}

TEST_CASE("REQ-X02: text fields keep the clipboard, undo and caret chords") {
    CHECK_FALSE(shouldDispatch(C("Ctrl+Z"), true));          // text undo, not show undo
    CHECK_FALSE(shouldDispatch(C("Ctrl+Y"), true));
    CHECK_FALSE(shouldDispatch(C("Ctrl+Shift+Z"), true));
    CHECK_FALSE(shouldDispatch(C("Ctrl+C"), true));
    CHECK_FALSE(shouldDispatch(C("Ctrl+V"), true));
    CHECK_FALSE(shouldDispatch(C("Ctrl+A"), true));
    CHECK_FALSE(shouldDispatch(C("Space"), true));
    CHECK_FALSE(shouldDispatch(C("Enter"), true));
    CHECK_FALSE(shouldDispatch(C("Backspace"), true));
    CHECK_FALSE(shouldDispatch(C("Left"), true));
    CHECK_FALSE(shouldDispatch(C("Ctrl+Left"), true));
    CHECK_FALSE(shouldDispatch(C("Shift+A"), true));         // capital letter
    CHECK_FALSE(shouldDispatch(C("Ctrl+Alt+E"), true));      // AltGr character
    // outside text fields all of them dispatch
    CHECK(shouldDispatch(C("Ctrl+Z"), false));
    CHECK(shouldDispatch(C("Space"), false));
    CHECK_FALSE(shouldDispatch(KeyChord{}, false));          // no key at all
}

TEST_CASE("REQ-X02: unbound chords are ignored with a reason") {
    const KeyMap km = KeyMap::defaults();
    auto r = km.dispatch(C("Ctrl+Q"), false);
    CHECK(r.kind == DispatchResult::Kind::Ignored);
    CHECK(std::string(r.ignoredBecause) == "unbound");
}

TEST_CASE("REQ-X02: conflict detection on remap") {
    KeyMap km = KeyMap::defaults();
    Binding b; b.chord = C("F5"); b.action = ActionId::PrevScene;
    auto s = km.bind(b);
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().detail.find("F5") != std::string::npos);
    CHECK(s.error().detail.find("Next scene") != std::string::npos);     // names what it collides with
    REQUIRE(km.conflictFor(C("F5")));
    CHECK(km.conflictFor(C("F5"))->action == ActionId::NextScene);
    CHECK(km.conflictFor(C("F9")) == nullptr);
    CHECK(km.lookup(C("F5"))->action == ActionId::NextScene);           // unchanged after the refusal
    // explicit replace resolves the conflict
    REQUIRE(km.bind(b, true).ok());
    CHECK(km.lookup(C("F5"))->action == ActionId::PrevScene);
    CHECK(km.bindingsFor(ActionId::NextScene).empty());
}

TEST_CASE("REQ-X02: users can remap an action to a new key") {
    KeyMap km = KeyMap::defaults();
    REQUIRE(km.rebind(C("F5"), C("F9")).ok());
    CHECK(km.lookup(C("F5")) == nullptr);
    CHECK(km.lookup(C("F9"))->action == ActionId::NextScene);
    // occupied target: refused, nothing changes
    CHECK_FALSE(km.rebind(C("F9"), C("F6")).ok());
    CHECK(km.lookup(C("F9"))->action == ActionId::NextScene);
    CHECK(km.lookup(C("F6"))->action == ActionId::Pause);
    CHECK_FALSE(km.rebind(C("F11"), C("F12")).ok());                    // nothing to move
    // bind an unbound action (value-bearing)
    Binding b; b.chord = C("Ctrl+1"); b.action = ActionId::SceneSelect; b.value = 7;
    CHECK(km.bind(b).ok());
    b.chord = C("Ctrl+2"); b.value = 9;                                  // out of range
    CHECK_FALSE(km.bind(b).ok());
    b.chord = C("Ctrl+3"); b.action = ActionId::NextScene; b.value = 0.5; // action takes no value
    CHECK_FALSE(km.bind(b).ok());
    CHECK(km.unbind(C("Ctrl+1")));
    CHECK_FALSE(km.unbind(C("Ctrl+1")));
    km.resetToDefaults();
    CHECK(km.lookup(C("F5"))->action == ActionId::NextScene);
}

TEST_CASE("REQ-X02: safety-critical actions cannot be bound to chords that stop working while typing") {
    KeyMap km = KeyMap::defaults();
    Binding b; b.chord = C("B"); b.action = ActionId::EmergencyBlackout;
    auto s = km.bind(b);
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().detail.find("typing") != std::string::npos);
    b.chord = C("Ctrl+Z"); CHECK_FALSE(km.bind(b, true).ok());
    b.chord = C("Space");  CHECK_FALSE(km.bind(b, true).ok());
    b.chord = C("F12");    CHECK(km.bind(b).ok());
    b.chord = C("Ctrl+B"); b.action = ActionId::VisualBlackout; CHECK(km.bind(b).ok());
    // non-critical actions may use any chord
    b.chord = C("B"); b.action = ActionId::NextScene; CHECK(km.bind(b).ok());
}

TEST_CASE("REQ-X02: bindings need exactly one target") {
    KeyMap km;
    Binding b; b.chord = C("F2");
    CHECK_FALSE(km.bind(b).ok());                            // neither action nor UI command
    b.action = ActionId::NextScene; b.ui = UiCommand::ShowKeyboardHelp;
    CHECK_FALSE(km.bind(b).ok());                            // both
    b.ui = UiCommand::None; b.chord = KeyChord{};
    CHECK_FALSE(km.bind(b).ok());                            // no key
    b.chord = C("F2"); b.action = static_cast<ActionId>(4242);
    CHECK_FALSE(km.bind(b).ok());                            // unknown action
}

TEST_CASE("REQ-X02: imported key sets resolve duplicate chords and report the rejects") {
    std::vector<Binding> list;
    Binding a; a.chord = C("F2"); a.action = ActionId::NextScene; list.push_back(a);
    Binding b; b.chord = C("F2"); b.action = ActionId::PrevScene; list.push_back(b);
    Binding c; c.chord = C("F3"); c.action = ActionId::Pause; list.push_back(c);
    std::vector<Binding> rejected;
    KeyMap km = KeyMap::fromBindings(list, &rejected);
    CHECK(km.lookup(C("F2"))->action == ActionId::NextScene);          // first claimant wins
    REQUIRE(rejected.size() == 1);
    CHECK(rejected[0].action == ActionId::PrevScene);
    CHECK(km.bindings().size() == 2);
}

TEST_CASE("REQ-X02: remaps persist as override lines and restore exactly") {
    KeyMap km = KeyMap::defaults();
    CHECK(km.exportOverrides().empty());                     // pristine map exports nothing
    REQUIRE(km.rebind(C("F5"), C("F9")).ok());
    Binding b; b.chord = C("Ctrl+L"); b.action = ActionId::Brightness; b.value = 0.5; REQUIRE(km.bind(b).ok());
    REQUIRE(km.unbind(C("Ctrl+Y")));
    const auto lines = km.exportOverrides();
    CHECK(lines.size() == 4);                                // F9 added, Ctrl+L added, F5 removed, Ctrl+Y removed
    KeyMap restored;
    REQUIRE(restored.importOverrides(lines).ok());
    CHECK(restored.lookup(C("F9"))->action == ActionId::NextScene);
    CHECK(restored.lookup(C("Ctrl+L"))->action == ActionId::Brightness);
    CHECK(restored.lookup(C("Ctrl+L"))->value == doctest::Approx(0.5));
    CHECK(restored.lookup(C("F5")) == nullptr);
    CHECK(restored.lookup(C("Ctrl+Y")) == nullptr);
    CHECK(restored.exportOverrides() == lines);
    // a bad line leaves the current map untouched
    KeyMap other = KeyMap::defaults();
    CHECK_FALSE(other.importOverrides({"F9=2:0", "garbage"}).ok());
    CHECK(other.lookup(C("F9")) == nullptr);
    CHECK_FALSE(other.importOverrides({"Hyper+Q=2:0"}).ok());
    CHECK_FALSE(other.importOverrides({"F9=B:0"}).ok());
}
