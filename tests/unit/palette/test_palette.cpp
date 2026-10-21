#include <algorithm>
#include "doctest/doctest.h"
#include "ss/palette/command_palette.hpp"
#include "ss/palette/help.hpp"

using namespace ss;
using namespace ss::palette;

namespace {
std::string highlighted(const std::string& text, const std::vector<Range>& rs) {
    std::string out;
    std::size_t pos = 0;
    for (const auto& r : rs) {
        out += text.substr(pos, r.begin - pos) + "[" + text.substr(r.begin, r.end - r.begin) + "]";
        pos = r.end;
    }
    return out + text.substr(pos);
}
std::vector<ActionId> ids(const std::vector<SearchResult>& r) {
    std::vector<ActionId> v;
    for (auto& x : r) v.push_back(x.id);
    return v;
}
}  // namespace

TEST_CASE("REQ-X02: fuzzy match is a case-insensitive subsequence match with highlight ranges") {
    auto m = fuzzyMatch("nxtsc", "Next scene");
    REQUIRE(m.has_value());
    CHECK(highlighted("Next scene", m->ranges) == "[N]e[xt] [sc]ene");
    CHECK_FALSE(fuzzyMatch("zzz", "Next scene").has_value());
    CHECK_FALSE(fuzzyMatch("scene next", "Next scene").has_value());      // order matters for a single subsequence
    CHECK_FALSE(fuzzyMatch("a", "").has_value());
    CHECK_FALSE(fuzzyMatch(std::string(65, 'a'), std::string(100, 'a')).has_value());   // bounded query
    CHECK(fuzzyMatch("", "anything").has_value());
    auto sp = fuzzyMatch("next sc", "Next scene");                        // spaces in the query are ignored
    REQUIRE(sp.has_value());
    CHECK(highlighted("Next scene", sp->ranges) == "[Next] [sc]ene");
}

TEST_CASE("REQ-X02: prefix and word-boundary matches outscore scattered matches") {
    const int prefix = fuzzyMatch("bri", "Brightness")->score;
    const int boundary = fuzzyMatch("bri", "Set brightness")->score;
    const int scattered = fuzzyMatch("bri", "Subscribe right")->score;
    CHECK(prefix > boundary);
    CHECK(boundary > scattered);
    CHECK(fuzzyMatch("next", "Next")->score > fuzzyMatch("nex", "Next")->score);     // exact beats prefix
    // camelCase boundaries count
    CHECK(fuzzyMatch("sb", "SceneBank")->score > fuzzyMatch("sb", "Disable")->score);
    // consecutive runs beat gaps
    CHECK(fuzzyMatch("scene", "Next scene")->score > fuzzyMatch("scene", "Select cue nine")->score);
}

TEST_CASE("REQ-X02: palette ranks actions - name prefix first, deterministic order") {
    CommandPalette p;
    auto r = p.search("blackout");
    REQUIRE(r.size() >= 3);
    // all three blackout actions plus restore are found
    std::vector<ActionId> got = ids(r);
    for (ActionId a : {ActionId::VisualBlackout, ActionId::LightingBlackout, ActionId::EmergencyBlackout, ActionId::RestoreBlackout})
        CHECK(std::find(got.begin(), got.end(), a) != got.end());
    // same query twice gives byte-identical results
    auto r2 = p.search("blackout");
    REQUIRE(r2.size() == r.size());
    for (std::size_t i = 0; i < r.size(); ++i) {
        CHECK(r[i].id == r2[i].id);
        CHECK(r[i].score == r2[i].score);
    }
    // scores never increase down the list; ties break by name then id
    for (std::size_t i = 1; i < r.size(); ++i) CHECK(r[i - 1].score >= r[i].score);
}

TEST_CASE("REQ-X02: typing 'next' puts Next scene first and typing 'brig' finds Brightness") {
    CommandPalette p;
    auto r = p.search("next");
    REQUIRE(!r.empty());
    CHECK(r[0].id == ActionId::NextScene);
    CHECK(highlighted(Registry::find(r[0].id)->name, r[0].nameRanges) == "[Next] scene");
    r = p.search("brig");
    REQUIRE(!r.empty());
    CHECK(r[0].id == ActionId::Brightness);
    CHECK(r[0].takesValue);
    r = p.search("emerg");
    REQUIRE(!r.empty());
    CHECK(r[0].id == ActionId::EmergencyBlackout);
    CHECK(r[0].safetyCritical);
}

TEST_CASE("REQ-X02: multi-word queries match in any order across name, keywords and category") {
    CommandPalette p;
    auto r = p.search("blackout lighting");
    REQUIRE(!r.empty());
    CHECK(r[0].id == ActionId::LightingBlackout);
    r = p.search("dmx arm");
    REQUIRE(!r.empty());
    CHECK(r[0].id == ActionId::ArmLighting);
    // keyword search: "panic" finds the emergency blackout though it is not in the name
    r = p.search("panic");
    REQUIRE(!r.empty());
    CHECK(r[0].id == ActionId::EmergencyBlackout);
    CHECK(r[0].field == MatchField::Keywords);
    CHECK(r[0].nameRanges.empty());
}

TEST_CASE("REQ-X02: recently used actions are boosted") {
    CommandPalette p;
    auto before = p.search("sna");               // snapshot store/recall A/B score within a few points of each other
    REQUIRE(before.size() >= 4);
    const ActionId last = before[3].id;
    CHECK(before[0].id != last);
    CHECK(p.recent().empty());
    p.noteUsed(last);
    auto after = p.search("sna");
    CHECK(after[0].id == last);
    CHECK(after[0].recent);
    CHECK(after[0].score > before[3].score);
    // most recent first, no duplicates, bounded
    p.noteUsed(ActionId::NextScene); p.noteUsed(last);
    REQUIRE(p.recent().size() == 2);
    CHECK(p.recent()[0] == last);
    for (int i = 1; i <= 120; ++i) p.noteUsed(static_cast<ActionId>(i));   // unknown ids are ignored
    CHECK(p.recent().size() <= CommandPalette::kMaxRecent);
    for (ActionId a : p.recent()) CHECK(Registry::find(a) != nullptr);
}

TEST_CASE("REQ-X02: recent boost does not beat a clearly better match") {
    CommandPalette p;
    p.noteUsed(ActionId::OpenShow);
    auto r = p.search("next scene");
    REQUIRE(!r.empty());
    CHECK(r[0].id == ActionId::NextScene);
}

TEST_CASE("REQ-X02: category filter restricts results") {
    CommandPalette p;
    SearchOptions o; o.category = Category::Lighting;
    auto r = p.search("", o);
    REQUIRE(r.size() == 3);
    for (auto& x : r) CHECK(Registry::find(x.id)->category == Category::Lighting);
    r = p.search("blackout", o);
    CHECK(r.empty());                                      // lighting blackout lives under Blackout
    o.category = Category::Blackout;
    r = p.search("blackout", o);
    CHECK(r.size() == 4);
    r = p.search("light", o);
    REQUIRE(!r.empty());
    CHECK(r[0].id == ActionId::LightingBlackout);
}

TEST_CASE("REQ-X02: empty query lists recent actions first, then everything by category and name") {
    CommandPalette p;
    p.noteUsed(ActionId::SaveShow);
    p.noteUsed(ActionId::Pause);
    auto r = p.search("");
    REQUIRE(r.size() == 50);                                // default limit
    CHECK(r[0].id == ActionId::Pause);
    CHECK(r[1].id == ActionId::SaveShow);
    CHECK(r[0].recent);
    SearchOptions all; all.limit = 1000;
    auto full = p.search("", all);
    CHECK(full.size() == Registry::all().size());
    CHECK(Registry::find(full[2].id)->category == Category::Scenes);       // then category order
    std::vector<ActionId> seen = ids(full);
    std::sort(seen.begin(), seen.end());
    CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());      // no duplicates
}

TEST_CASE("REQ-X02: result limit is honoured") {
    CommandPalette p;
    SearchOptions o; o.limit = 3;
    CHECK(p.search("s", o).size() == 3);
    CHECK(p.search("e", o).size() == 3);
}

TEST_CASE("REQ-X02: results show the effective shortcut from the keymap") {
    KeyMap km = KeyMap::defaults();
    CommandPalette p(&km);
    auto r = p.search("next scene");
    REQUIRE(!r.empty());
    CHECK(r[0].shortcut == "F5");
    r = p.search("select scene pad");
    REQUIRE(!r.empty());
    CHECK(r[0].id == ActionId::SceneSelect);
    CHECK(r[0].shortcut == "1-8");
    REQUIRE(km.rebind(*parseChord("F5"), *parseChord("F9")).ok());
    r = p.search("next scene");
    CHECK(r[0].shortcut == "F9");
    CommandPalette noMap;
    CHECK(noMap.search("next scene")[0].shortcut == "F5");                  // registry default without a keymap
}

TEST_CASE("REQ-X02: recent list restore drops unknown and duplicate ids") {
    CommandPalette p;
    p.setRecent({ActionId::Undo, static_cast<ActionId>(4242), ActionId::Undo, ActionId::Redo, ActionId::None});
    REQUIRE(p.recent().size() == 2);
    CHECK(p.recent()[0] == ActionId::Undo);
    CHECK(p.recent()[1] == ActionId::Redo);
}

TEST_CASE("REQ-X02: highlight ranges are valid byte ranges inside the name") {
    CommandPalette p;
    for (const char* q : {"s", "ne", "bl", "rec", "ca", "bpm", "key", "x"}) {
        for (const auto& r : p.search(q, SearchOptions{std::nullopt, 1000})) {
            const std::string name = Registry::find(r.id)->name;
            std::size_t prevEnd = 0;
            for (const auto& rg : r.nameRanges) {
                CHECK(rg.begin < rg.end);
                CHECK(rg.end <= name.size());
                CHECK(rg.begin >= prevEnd);                // sorted, non-overlapping
                prevEnd = rg.end;
            }
        }
    }
}

TEST_CASE("REQ-X02: every action can be found by typing its own name") {
    CommandPalette p;
    for (const auto& a : Registry::all()) {
        auto r = p.search(a.name);
        INFO(a.name);
        REQUIRE(!r.empty());
        CHECK(r[0].id == a.id);
    }
}

TEST_CASE("REQ-D11: brightness reachable by keyboard through the palette with a typed value") {
    CommandPalette p;
    auto r = p.search("brightness");
    REQUIRE(!r.empty());
    CHECK(r[0].id == ActionId::Brightness);
    auto a = makeAction(r[0].id, "40%", ActionSource::Keyboard);
    REQUIRE(a.ok());
    CHECK(a.value().value == doctest::Approx(0.4));
    CHECK(a.value().source == ActionSource::Keyboard);
}
