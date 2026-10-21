// StageSignal palette: Command Palette (Ctrl+K) fuzzy search and ranking. Fully deterministic: same query, same
// recent list and same registry give the same ordered result on every platform.
#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include "ss/palette/keymap.hpp"
#include "ss/palette/registry.hpp"

namespace ss::palette {

struct Range { std::size_t begin = 0, end = 0; };      // byte offsets [begin, end) into the display text

struct FuzzyMatch {
    int score = 0;
    std::vector<Range> ranges;                           // merged runs of matched characters
};
// Case-insensitive (ASCII) subsequence match with the best alignment: prefix, word-boundary and consecutive-run
// boosts, small gap penalties. Spaces in the query are ignored. nullopt when the query is not a subsequence.
std::optional<FuzzyMatch> fuzzyMatch(const std::string& query, const std::string& text);

struct SearchOptions {
    std::optional<Category> category;                    // category filter
    std::size_t limit = 50;
};

enum class MatchField : std::uint8_t { None, Name, Keywords, Category, Description };

struct SearchResult {
    ActionId id = ActionId::None;
    int score = 0;
    MatchField field = MatchField::None;
    std::vector<Range> nameRanges;                       // highlight ranges within ActionInfo::name (empty unless field == Name)
    bool recent = false;
    std::string shortcut;                                // effective shortcut from the keymap ("" when unbound)
    bool takesValue = false;
    bool safetyCritical = false;
};

class CommandPalette {
public:
    static constexpr std::size_t kMaxRecent = 12;
    explicit CommandPalette(const KeyMap* keymap = nullptr) : keymap_(keymap) {}
    void setKeyMap(const KeyMap* k) { keymap_ = k; }
    void noteUsed(ActionId a);                            // moves to the front of the recently-used list
    const std::vector<ActionId>& recent() const { return recent_; }
    void setRecent(const std::vector<ActionId>& r);       // restore from settings (unknown ids are dropped)
    // Empty query lists recent actions first, then everything by category and name.
    std::vector<SearchResult> search(const std::string& query, const SearchOptions& opt = {}) const;
private:
    const KeyMap* keymap_;
    std::vector<ActionId> recent_;
};

}  // namespace ss::palette
