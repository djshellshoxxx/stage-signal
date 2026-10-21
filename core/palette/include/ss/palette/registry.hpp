// StageSignal palette: action registry metadata (IF-02). Every ss::ActionId has exactly one entry; the completeness
// test parses core/base/include/ss/base/actions.hpp and fails when a new id has no metadata.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/actions.hpp"
#include "ss/base/error.hpp"
#include "ss/base/types.hpp"

namespace ss::palette {

enum class Category : std::uint8_t {
    Scenes, Blackout, Look, Tempo, Text, Safety, Lighting, Recording, Effects, Edit, Output, File
};
constexpr int kCategoryCount = 12;
const char* categoryName(Category c);
// Case-insensitive lookup of a category by its display name ("lighting"); false when unknown.
bool parseCategory(const std::string& name, Category& out);

enum class ValueKind : std::uint8_t {
    None,         // action takes no value
    Normalized,   // 0..1 (typed as 0.5 or 50%)
    SceneSlot,    // integer 0..7 on the current bank (typed as 1..8 in the UI: see ValueSpec::uiOffset)
    Mode,         // 0 Auto, 1 Programmed, 2 Hybrid
    Bpm,          // beats per minute, 20..300
    Index,        // non-negative integer id (cue index, text id, output id)
    KeyIndex      // 0..11 major tonic C..B, 12..23 minor tonic C..B
};

struct ValueSpec {
    bool takesValue = false;
    ValueKind kind = ValueKind::None;
    const char* unit = "";          // "", "%", "BPM", "slot", "cue", "key", ...
    double min = 0.0, max = 0.0;
    bool integer = false;
    double defaultValue = 0.0;
    int uiOffset = 0;               // value typed by the operator = stored value + uiOffset (scene pads are shown 1..8)
};

struct ActionInfo {
    ActionId id = ActionId::None;
    const char* ident = "";         // enumerator name in actions.hpp, used by the completeness test
    const char* name = "";          // display name
    Category category = Category::Scenes;
    const char* description = "";
    ValueSpec value;
    const char* defaultShortcut = "";   // display text; "" = palette only. SceneSelect shows "1-8"
    const char* keywords = "";      // extra search words, space separated
    bool safetyCritical = false;    // mirrors ss::isSafetyCritical; never rate-limited or dropped
};

class Registry {
public:
    static const std::vector<ActionInfo>& all();            // ordered by numeric id
    static const ActionInfo* find(ActionId id);              // nullptr for None / unknown values
    static const ActionInfo* findByIdent(const std::string& ident);
};

// Parses operator-typed value text for an action: "0.5", "50%" for Normalized; "3" for scene pads (1..8 on screen);
// "128" for Bpm; mode names "auto|programmed|hybrid" or 0..2. Out-of-range input is an error, never clamped silently.
Result<double> parseValue(const ActionInfo& info, const std::string& text);
// Builds the IF-02 action for a registry entry (source Ui). Fails for unknown ids or bad value text.
Result<Action> makeAction(ActionId id, const std::string& valueText = "", ActionSource source = ActionSource::Ui);

}  // namespace ss::palette
