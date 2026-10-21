#include "doctest/doctest.h"
#include "ss/palette/help.hpp"

using namespace ss;
using namespace ss::palette;

namespace {
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
}

TEST_CASE("REQ-X02: keyboard help lists every default shortcut with text labels") {
    const std::string h = keyboardHelpText(KeyMap::defaults());
    CHECK(has(h, "F1"));
    CHECK(has(h, "Keyboard shortcuts"));
    CHECK(has(h, "F5"));      CHECK(has(h, "Next scene"));
    CHECK(has(h, "F6"));      CHECK(has(h, "Pause automation"));
    CHECK(has(h, "F7"));      CHECK(has(h, "Resume automation"));
    CHECK(has(h, "F8"));      CHECK(has(h, "Emergency blackout"));
    CHECK(has(h, "Shift+F8"));CHECK(has(h, "Restore from blackout"));
    CHECK(has(h, "Ctrl+S"));  CHECK(has(h, "Save show"));
    CHECK(has(h, "Ctrl+K"));  CHECK(has(h, "Open command palette"));
    CHECK(has(h, "Ctrl+Z"));  CHECK(has(h, "Ctrl+Y"));
    CHECK(has(h, "1-8"));     CHECK(has(h, "Select scene pad"));
    // state is carried by text, not colour
    CHECK(has(h, "[SAFETY]"));
    CHECK(has(h, "(takes a value)"));
    // the typing rule and the not-global note
    CHECK(has(h, "scene pads never fire while you type"));
    CHECK(has(h, "not global"));
    CHECK(has(h, "Command Palette (Ctrl+K)"));
}

TEST_CASE("REQ-X02: help is deterministic and follows user remaps") {
    KeyMap km = KeyMap::defaults();
    const std::string a = keyboardHelpText(km), b = keyboardHelpText(km);
    CHECK(a == b);
    REQUIRE(km.rebind(*parseChord("F8"), *parseChord("F12")).ok());
    const std::string h = keyboardHelpText(km);
    CHECK(has(h, "F12"));
    CHECK_FALSE(has(h, "  F8 "));
    REQUIRE(km.unbind(*parseChord("Ctrl+K")));
    CHECK(has(keyboardHelpText(km), "Command Palette (unbound"));
}

TEST_CASE("REQ-X02: help rows group by category and mark safety-critical actions") {
    const auto rows = keyboardHelpRows(KeyMap::defaults());
    REQUIRE(!rows.empty());
    for (std::size_t i = 1; i < rows.size(); ++i)
        CHECK(static_cast<int>(rows[i - 1].category) <= static_cast<int>(rows[i].category));
    bool sawEmergency = false;
    for (const auto& r : rows) {
        if (r.action == "Emergency blackout") { sawEmergency = true; CHECK(r.safetyCritical); CHECK(r.keys == "F8"); }
        if (r.action == "Next scene") CHECK_FALSE(r.safetyCritical);
    }
    CHECK(sawEmergency);
}

TEST_CASE("REQ-X02: shortcut text collapses digit pads and joins multiple chords") {
    KeyMap km = KeyMap::defaults();
    CHECK(shortcutText(km, ActionId::SceneSelect) == "1-8");
    CHECK(shortcutText(km, ActionId::NextScene) == "F5");
    CHECK(shortcutText(km, ActionId::TapTempo).empty());
    Binding b; b.chord = *parseChord("Alt+N"); b.action = ActionId::NextScene;
    REQUIRE(km.bind(b).ok());
    CHECK(shortcutText(km, ActionId::NextScene) == "Alt+N, F5");
    km.unbind(*parseChord("4"));
    CHECK(shortcutText(km, ActionId::SceneSelect) == "1-3, 5-8");
    km.unbind(*parseChord("2"));
    km.unbind(*parseChord("3"));
    CHECK(shortcutText(km, ActionId::SceneSelect) == "1, 5-8");
}

TEST_CASE("REQ-X02: every action is either listed with a shortcut or reachable from the palette") {
    const KeyMap km = KeyMap::defaults();
    const std::string h = keyboardHelpText(km);
    for (const auto& a : Registry::all()) {
        INFO(a.name);
        CHECK(has(h, a.name));
    }
}
