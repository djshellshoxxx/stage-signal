#include "ss/palette/command_palette.hpp"
#include <algorithm>
#include <cctype>
#include <climits>
#include "ss/palette/help.hpp"

namespace ss::palette {

namespace {

constexpr int kNeg = INT_MIN / 4;
constexpr int kPrefixBonus = 40, kBoundaryBonus = 25, kConsecutiveBonus = 20, kCharScore = 10;
constexpr int kExactBonus = 100, kStartsWithBonus = 30, kShortcutBonus = 6;
constexpr std::size_t kMaxQuery = 64;

char lc(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool isBoundary(const std::string& t, std::size_t j) {
    if (j == 0) return true;
    const unsigned char p = static_cast<unsigned char>(t[j - 1]), c = static_cast<unsigned char>(t[j]);
    if (!std::isalnum(p)) return true;
    return std::islower(p) && std::isupper(c);
}

std::vector<Range> mergeRanges(std::vector<Range> r) {
    std::sort(r.begin(), r.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
    std::vector<Range> out;
    for (const auto& x : r) {
        if (!out.empty() && x.begin <= out.back().end) out.back().end = std::max(out.back().end, x.end);
        else out.push_back(x);
    }
    return out;
}

std::vector<std::string> tokens(const std::string& q) {
    std::vector<std::string> t;
    std::string cur;
    for (char c : q) {
        if (std::isspace(static_cast<unsigned char>(c))) { if (!cur.empty()) { t.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) t.push_back(cur);
    return t;
}

struct FieldHit { int score = 0; MatchField field = MatchField::None; std::vector<Range> ranges; };

// Best weighted hit of one token across the searchable fields of an action.
std::optional<FieldHit> bestField(const std::string& tok, const ActionInfo& a) {
    struct F { MatchField f; std::string text; int weightPct; };
    const F fields[] = {
        {MatchField::Name, a.name, 100}, {MatchField::Keywords, a.keywords, 60},
        {MatchField::Category, categoryName(a.category), 40}, {MatchField::Description, a.description, 25},
    };
    std::optional<FieldHit> best;
    for (const auto& f : fields) {
        if (f.text.empty()) continue;
        auto m = fuzzyMatch(tok, f.text);
        if (!m) continue;
        const int s = m->score * f.weightPct / 100;
        if (!best || s > best->score) {
            FieldHit h; h.score = s; h.field = f.f;
            if (f.f == MatchField::Name) h.ranges = m->ranges;
            best = std::move(h);
        }
    }
    return best;
}

}  // namespace

std::optional<FuzzyMatch> fuzzyMatch(const std::string& query, const std::string& text) {
    std::string q;
    for (char c : query) if (!std::isspace(static_cast<unsigned char>(c))) q += lc(c);
    FuzzyMatch res;
    if (q.empty()) return res;
    const std::size_t m = q.size(), n = text.size();
    if (m > kMaxQuery || m > n) return std::nullopt;
    std::string t(n, ' ');
    for (std::size_t i = 0; i < n; ++i) t[i] = lc(text[i]);

    std::vector<int> dp(m * n, kNeg), par(m * n, -1);
    auto charScore = [&](std::size_t j) {
        return kCharScore + (j == 0 ? kPrefixBonus : isBoundary(text, j) ? kBoundaryBonus : 0);
    };
    for (std::size_t i = 0; i < m; ++i) {
        int runBest = kNeg, runK = -1;
        for (std::size_t j = 0; j < n; ++j) {
            if (i > 0 && j >= 2) {                                   // gap candidates k <= j-2: dp[k] + k - (j-1)
                const int d = dp[(i - 1) * n + (j - 2)];
                if (d > kNeg && d + static_cast<int>(j - 2) > runBest) { runBest = d + static_cast<int>(j - 2); runK = static_cast<int>(j - 2); }
            }
            if (t[j] != q[i]) continue;
            if (i == 0) {
                dp[j] = charScore(j) - static_cast<int>(std::min<std::size_t>(j, 10));   // leading gap penalty
                continue;
            }
            int bestPrev = kNeg, bestK = -1;
            if (j >= 1) {
                const int d = dp[(i - 1) * n + (j - 1)];
                if (d > kNeg) { bestPrev = d + kConsecutiveBonus; bestK = static_cast<int>(j - 1); }
            }
            if (runK >= 0 && runBest - static_cast<int>(j - 1) > bestPrev) { bestPrev = runBest - static_cast<int>(j - 1); bestK = runK; }
            if (bestK < 0) continue;
            dp[i * n + j] = charScore(j) + bestPrev;
            par[i * n + j] = bestK;
        }
    }
    int best = kNeg; int bj = -1;
    for (std::size_t j = 0; j < n; ++j) {
        const int d = dp[(m - 1) * n + j];
        if (d > best) { best = d; bj = static_cast<int>(j); }
    }
    if (bj < 0) return std::nullopt;
    std::vector<std::size_t> pos(m);
    int j = bj;
    for (std::size_t i = m; i-- > 0;) {
        pos[i] = static_cast<std::size_t>(j);
        j = par[i * n + static_cast<std::size_t>(j)];
    }
    for (std::size_t i = 0; i < m;) {
        std::size_t e = i;
        while (e + 1 < m && pos[e + 1] == pos[e] + 1) ++e;
        res.ranges.push_back({pos[i], pos[e] + 1});
        i = e + 1;
    }
    res.score = best;
    // whole-text bonuses
    std::string tq;
    for (char c : query) if (!std::isspace(static_cast<unsigned char>(c))) tq += lc(c);
    std::string tt;
    for (char c : text) if (!std::isspace(static_cast<unsigned char>(c))) tt += lc(c);
    if (tq == tt) res.score += kExactBonus;
    else if (tt.compare(0, tq.size(), tq) == 0) res.score += kStartsWithBonus;
    return res;
}

void CommandPalette::noteUsed(ActionId a) {
    if (!Registry::find(a)) return;
    recent_.erase(std::remove(recent_.begin(), recent_.end(), a), recent_.end());
    recent_.insert(recent_.begin(), a);
    if (recent_.size() > kMaxRecent) recent_.resize(kMaxRecent);
}

void CommandPalette::setRecent(const std::vector<ActionId>& r) {
    recent_.clear();
    for (ActionId a : r) {
        if (!Registry::find(a) || std::find(recent_.begin(), recent_.end(), a) != recent_.end()) continue;
        recent_.push_back(a);
        if (recent_.size() >= kMaxRecent) break;
    }
}

std::vector<SearchResult> CommandPalette::search(const std::string& query, const SearchOptions& opt) const {
    std::vector<SearchResult> out;
    const auto toks = tokens(query);
    auto make = [&](const ActionInfo& a, int score, MatchField f, std::vector<Range> ranges, bool recent) {
        SearchResult r;
        r.id = a.id; r.score = score; r.field = f; r.nameRanges = std::move(ranges); r.recent = recent;
        r.takesValue = a.value.takesValue; r.safetyCritical = a.safetyCritical;
        r.shortcut = keymap_ ? shortcutText(*keymap_, a.id) : std::string(a.defaultShortcut);
        return r;
    };
    auto recentRank = [&](ActionId a) -> int {
        auto it = std::find(recent_.begin(), recent_.end(), a);
        return it == recent_.end() ? -1 : static_cast<int>(it - recent_.begin());
    };

    if (toks.empty()) {
        for (std::size_t i = 0; i < recent_.size(); ++i) {
            const ActionInfo* a = Registry::find(recent_[i]);
            if (a && (!opt.category || *opt.category == a->category))
                out.push_back(make(*a, 1000 - static_cast<int>(i), MatchField::None, {}, true));
        }
        std::vector<const ActionInfo*> rest;
        for (const auto& a : Registry::all())
            if (recentRank(a.id) < 0 && (!opt.category || *opt.category == a.category)) rest.push_back(&a);
        std::sort(rest.begin(), rest.end(), [](const ActionInfo* x, const ActionInfo* y) {
            if (x->category != y->category) return static_cast<int>(x->category) < static_cast<int>(y->category);
            const std::string xn = x->name, yn = y->name;
            return xn != yn ? xn < yn : id(x->id) < id(y->id);
        });
        for (const auto* a : rest) out.push_back(make(*a, 0, MatchField::None, {}, false));
        if (out.size() > opt.limit) out.resize(opt.limit);
        return out;
    }

    for (const auto& a : Registry::all()) {
        if (opt.category && *opt.category != a.category) continue;
        int score = kNeg; MatchField field = MatchField::None; std::vector<Range> ranges;
        // (1) whole query as one subsequence of the name keeps word order ("next sc")
        std::string whole;
        for (const auto& t : toks) whole += t;
        if (auto m = fuzzyMatch(whole, a.name)) { score = m->score; field = MatchField::Name; ranges = m->ranges; }
        // (2) every token somewhere (name, keywords, category, description) in any order ("blackout light")
        {
            int sum = 0; bool ok = true; MatchField f0 = MatchField::None; std::vector<Range> rs;
            for (const auto& tk : toks) {
                auto h = bestField(tk, a);
                if (!h) { ok = false; break; }
                sum += h->score;
                if (f0 == MatchField::None || (h->field == MatchField::Name && f0 != MatchField::Name)) f0 = h->field;
                rs.insert(rs.end(), h->ranges.begin(), h->ranges.end());
            }
            if (ok) {
                const int s = sum / static_cast<int>(toks.size());
                if (s > score) { score = s; field = f0; ranges = f0 == MatchField::Name ? mergeRanges(rs) : std::vector<Range>{}; }
            }
        }
        if (score == kNeg) continue;
        const int rank = recentRank(a.id);
        if (rank >= 0) score += std::max(4, 40 - 3 * rank);
        SearchResult res = make(a, score, field, std::move(ranges), rank >= 0);
        if (!res.shortcut.empty()) res.score += kShortcutBonus;    // primary live actions win ties over palette-only ones
        out.push_back(std::move(res));
    }
    std::sort(out.begin(), out.end(), [](const SearchResult& x, const SearchResult& y) {
        if (x.score != y.score) return x.score > y.score;
        const ActionInfo* a = Registry::find(x.id); const ActionInfo* b = Registry::find(y.id);
        const std::string an = a->name, bn = b->name;
        if (an != bn) return an < bn;
        return id(x.id) < id(y.id);
    });
    if (out.size() > opt.limit) out.resize(opt.limit);
    return out;
}

}  // namespace ss::palette
