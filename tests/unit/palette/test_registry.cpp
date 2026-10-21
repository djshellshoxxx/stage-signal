#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include "doctest/doctest.h"
#include "ss/palette/keymap.hpp"
#include "ss/palette/registry.hpp"

using namespace ss;
using namespace ss::palette;

namespace {
struct EnumEntry { std::string ident; std::uint32_t value; };

// Reads the enumerators of `enum class ActionId` straight from the shared header.
std::vector<EnumEntry> actionsFromHeader() {
    std::ifstream f(SS_ACTIONS_HPP);
    REQUIRE_MESSAGE(f.good(), "cannot open " SS_ACTIONS_HPP);
    std::stringstream ss; ss << f.rdbuf();
    std::string text = ss.str();
    const auto a = text.find("enum class ActionId");
    REQUIRE(a != std::string::npos);
    const auto open = text.find('{', a), close = text.find("};", open);
    REQUIRE(open != std::string::npos);
    REQUIRE(close != std::string::npos);
    std::string body = text.substr(open + 1, close - open - 1);
    body = std::regex_replace(body, std::regex("//[^\n]*"), "");         // drop comments (they contain '=')
    std::vector<EnumEntry> out;
    const std::regex re("([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*([0-9]+)");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re); it != std::sregex_iterator(); ++it)
        out.push_back({(*it)[1], static_cast<std::uint32_t>(std::stoul((*it)[2]))});
    return out;
}
}  // namespace

TEST_CASE("REQ-X02: registry completeness - every ActionId in base/actions.hpp has metadata") {
    const auto entries = actionsFromHeader();
    REQUIRE(entries.size() > 40);                       // the parser really saw the enum
    int checked = 0;
    for (const auto& e : entries) {
        if (e.ident == "None") { CHECK(Registry::find(static_cast<ActionId>(e.value)) == nullptr); continue; }
        const ActionInfo* info = Registry::find(static_cast<ActionId>(e.value));
        INFO("ActionId " << e.ident << " = " << e.value << " has no palette metadata: add it to core/palette/src/registry.cpp");
        REQUIRE(info != nullptr);
        CHECK(std::string(info->ident) == e.ident);      // metadata is attached to the right id
        ++checked;
    }
    CHECK(checked == static_cast<int>(Registry::all().size()));    // and no phantom entries
}

TEST_CASE("REQ-X02: registry metadata is complete, unique and sane") {
    std::set<std::string> names;
    std::set<std::uint32_t> ids;
    for (const auto& a : Registry::all()) {
        INFO(a.ident);
        CHECK(std::string(a.name).size() >= 3);
        CHECK(std::string(a.description).size() >= 10);
        CHECK(names.insert(a.name).second);              // display names are unique (palette results are unambiguous)
        CHECK(ids.insert(id(a.id)).second);
        CHECK(static_cast<int>(a.category) < kCategoryCount);
        CHECK(a.safetyCritical == isSafetyCritical(a.id));
        if (a.value.takesValue) {
            CHECK(a.value.kind != ValueKind::None);
            CHECK(a.value.min <= a.value.max);
            CHECK(a.value.defaultValue >= a.value.min);
            CHECK(a.value.defaultValue <= a.value.max);
            CHECK(std::string(a.value.unit).size() > 0);
        } else {
            CHECK(a.value.kind == ValueKind::None);
        }
    }
    // ordered by id so find() can binary-search
    const auto& all = Registry::all();
    for (std::size_t i = 1; i < all.size(); ++i) CHECK(id(all[i - 1].id) < id(all[i].id));
}

TEST_CASE("REQ-X02: safety-critical flag marks the blackout and stop family") {
    for (ActionId a : {ActionId::EmergencyBlackout, ActionId::VisualBlackout, ActionId::LightingBlackout,
                       ActionId::RestoreBlackout, ActionId::DisarmLighting, ActionId::StopRecording,
                       ActionId::StopBroadcast, ActionId::DisableStrobing})
        CHECK(Registry::find(a)->safetyCritical);
    CHECK_FALSE(Registry::find(ActionId::NextScene)->safetyCritical);
    CHECK_FALSE(Registry::find(ActionId::Brightness)->safetyCritical);
}

TEST_CASE("REQ-X02: default shortcut metadata matches the default keymap") {
    const KeyMap km = KeyMap::defaults();
    for (const auto& a : Registry::all()) {
        const auto bs = km.bindingsFor(a.id);
        const std::string s = a.defaultShortcut;
        INFO(a.ident);
        if (bs.empty()) { CHECK(s.empty()); continue; }
        if (a.id == ActionId::SceneSelect) { CHECK(s == "1-8"); CHECK(bs.size() == 8); continue; }
        REQUIRE(bs.size() == 1);
        CHECK(s == chordString(bs[0].chord));
    }
}

TEST_CASE("REQ-D11: value metadata - brightness, speed, intensity, colour and effects take 0..1") {
    for (ActionId a : {ActionId::Brightness, ActionId::Speed, ActionId::Intensity, ActionId::ColorHue,
                       ActionId::ColorSaturation, ActionId::EffectPrismMirror, ActionId::EffectBloomPulse}) {
        const ActionInfo* i = Registry::find(a);
        REQUIRE(i);
        CHECK(i->value.takesValue);
        CHECK(i->value.kind == ValueKind::Normalized);
        CHECK(i->value.min == 0.0);
        CHECK(i->value.max == 1.0);
    }
    CHECK(Registry::find(ActionId::SetBpm)->value.unit == std::string("BPM"));
    CHECK(Registry::find(ActionId::SetBpm)->value.min == 20.0);
    CHECK(Registry::find(ActionId::SetBpm)->value.max == 300.0);
    CHECK_FALSE(Registry::find(ActionId::NextScene)->value.takesValue);
}

TEST_CASE("REQ-D11: operator-typed values parse, scale and range-check") {
    const ActionInfo& br = *Registry::find(ActionId::Brightness);
    CHECK(parseValue(br, "0.25").value() == doctest::Approx(0.25));
    CHECK(parseValue(br, "50%").value() == doctest::Approx(0.5));
    CHECK(parseValue(br, " 100 % ").value() == doctest::Approx(1.0));
    CHECK(parseValue(br, "").value() == doctest::Approx(0.5));            // default
    CHECK_FALSE(parseValue(br, "1.5").ok());
    CHECK_FALSE(parseValue(br, "-0.1").ok());
    CHECK_FALSE(parseValue(br, "abc").ok());
    CHECK_FALSE(parseValue(br, "nan").ok());
    CHECK_FALSE(parseValue(br, "150%").ok());                              // never clamped silently
    const ActionInfo& pad = *Registry::find(ActionId::SceneSelect);
    CHECK(parseValue(pad, "1").value() == 0.0);                            // pads are typed 1..8, stored 0..7
    CHECK(parseValue(pad, "8").value() == 7.0);
    CHECK_FALSE(parseValue(pad, "9").ok());
    CHECK_FALSE(parseValue(pad, "0").ok());
    CHECK_FALSE(parseValue(pad, "2.5").ok());
    const ActionInfo& bpm = *Registry::find(ActionId::SetBpm);
    CHECK(parseValue(bpm, "128").value() == 128.0);
    CHECK_FALSE(parseValue(bpm, "10").ok());
    CHECK_FALSE(parseValue(bpm, "50%").ok());
    const ActionInfo& mode = *Registry::find(ActionId::SetMode);
    CHECK(parseValue(mode, "hybrid").value() == 2.0);
    CHECK(parseValue(mode, "0").value() == 0.0);
    CHECK_FALSE(parseValue(mode, "3").ok());
    CHECK(parseValue(*Registry::find(ActionId::NextScene), "ignored").value() == 0.0);
}

TEST_CASE("REQ-D11: makeAction builds IF-02 actions with the right source and value") {
    auto a = makeAction(ActionId::Brightness, "75%");
    REQUIRE(a.ok());
    CHECK(a.value().actionId == id(ActionId::Brightness));
    CHECK(a.value().value == doctest::Approx(0.75));
    CHECK(a.value().source == ActionSource::Ui);
    CHECK(makeAction(ActionId::NextScene).ok());
    CHECK_FALSE(makeAction(ActionId::None).ok());
    CHECK_FALSE(makeAction(static_cast<ActionId>(9999)).ok());
    CHECK_FALSE(makeAction(ActionId::Brightness, "loud").ok());
    CHECK(makeAction(ActionId::Speed, "0.5", ActionSource::Remote).value().source == ActionSource::Remote);
}

TEST_CASE("REQ-X02: category names parse for the palette filter") {
    Category c;
    CHECK(parseCategory("lighting", c));
    CHECK(c == Category::Lighting);
    CHECK(parseCategory("Blackout", c));
    CHECK(c == Category::Blackout);
    CHECK(parseCategory("rec", c));
    CHECK(c == Category::Recording);
    CHECK_FALSE(parseCategory("nonsense", c));
    for (int i = 0; i < kCategoryCount; ++i) {
        Category back;
        CHECK(parseCategory(categoryName(static_cast<Category>(i)), back));
        CHECK(static_cast<int>(back) == i);
    }
}
