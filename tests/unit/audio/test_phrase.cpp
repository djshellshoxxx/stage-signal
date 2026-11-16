// FEAT-001: PhraseTracker (bar-in-phrase, phrase boundaries, pre-drop, confidence gate, nudge, manual start).
#include <cmath>
#include <vector>

#include "doctest/doctest.h"
#include "ss/audio/phrase_tracker.hpp"

using namespace ss::audio;

namespace {

// Arc model: a Drop is entered at every bar with (b % period == dropAt); the Drop lasts two bars.
// Build covers the two bars before each drop, so the last bar of a 8-bar phrase is Build when dropAt == 0.
EnergyPhase arc8(std::int64_t b) {
    const std::int64_t m = ((b % 8) + 8) % 8;
    if (m == 0 || m == 1) return EnergyPhase::Drop;
    if (m == 6 || m == 7) return EnergyPhase::Build;
    return EnergyPhase::Steady;
}

template <class PhaseFn>
void run(PhraseTracker& t, std::int64_t from, std::int64_t to, PhaseFn phase, float conf) {
    for (std::int64_t b = from; b < to; ++b) t.onBar(b, phase(b), conf);
}

PhraseParams params(int bars) {
    PhraseParams p;
    p.phraseBars = bars;
    return p;
}

}  // namespace

TEST_CASE("FEAT-001: invalid phrase parameters are rejected and the previous ones kept") {
    PhraseTracker t;
    CHECK(t.setParams(params(8)).ok());
    PhraseParams bad = params(5);
    CHECK_FALSE(t.setParams(bad).ok());
    CHECK(t.params().phraseBars == 8);
    bad = params(8);
    bad.minVotes = 0;
    CHECK_FALSE(t.setParams(bad).ok());
    bad = params(8);
    bad.lockShare = 0.2f;
    CHECK_FALSE(t.setParams(bad).ok());
    bad = params(8);
    bad.minConfidence = 1.5f;
    CHECK_FALSE(t.setParams(bad).ok());
    CHECK(t.params().minVotes == PhraseParams{}.minVotes);
}

TEST_CASE("FEAT-001: no phrase lock before enough drop votes, then bar-in-phrase and boundaries are exact") {
    PhraseTracker t;
    REQUIRE(t.setParams(params(8)).ok());
    run(t, 0, 16, arc8, 0.9f);      // first drop entry at bar 0 (not yet 3 votes)
    CHECK_FALSE(t.state().locked);
    CHECK(t.state().barInPhrase == -1);
    run(t, 16, 32, arc8, 0.9f);     // entries at 8, 16, 24, ... enough votes by bar 24
    const PhraseState s = t.state();
    CHECK(s.locked);
    CHECK(s.phraseBars == 8);
    CHECK(s.lockShare >= 0.99f);
    for (std::int64_t b = 32; b < 60; ++b) {
        t.onBar(b, arc8(b), 0.9f);
        const PhraseState st = t.state();
        CHECK(st.barInPhrase == static_cast<int>(b % 8));
        CHECK(st.phraseStart == (b % 8 == 0));
        CHECK(st.barsToPhraseStart == 8 - static_cast<int>(b % 8));
        CHECK(st.nextPhraseStartBar == b + (8 - b % 8));
    }
}

TEST_CASE("FEAT-001: pre-drop is flagged only on the last bar of a phrase while the arc is in Build") {
    PhraseTracker t;
    REQUIRE(t.setParams(params(8)).ok());
    run(t, 0, 40, arc8, 0.9f);
    int preDrops = 0;
    for (std::int64_t b = 40; b < 80; ++b) {
        t.onBar(b, arc8(b), 0.9f);
        const PhraseState st = t.state();
        CHECK(st.preDrop == (b % 8 == 7));
        if (st.preDrop) ++preDrops;
    }
    CHECK(preDrops == 5);
}

TEST_CASE("FEAT-001: 16-bar and 4-bar phrases follow the same drops, aligned to the drop residue") {
    // Drops every 16 bars at bar 4 mod 16: a 16-bar phrase starts at 4 mod 16, a 4-bar phrase starts at 0 mod 4.
    auto arc16 = [](std::int64_t b) {
        const std::int64_t m = ((b % 16) + 16) % 16;
        return (m == 4 || m == 5) ? EnergyPhase::Drop : EnergyPhase::Steady;
    };
    PhraseTracker t16;
    REQUIRE(t16.setParams(params(16)).ok());
    run(t16, 0, 100, arc16, 0.9f);
    CHECK(t16.state().locked);
    for (std::int64_t b = 100; b < 150; ++b) {
        t16.onBar(b, arc16(b), 0.9f);
        CHECK(t16.state().phraseStart == (b % 16 == 4));
    }

    PhraseTracker t4;
    REQUIRE(t4.setParams(params(4)).ok());
    run(t4, 0, 100, arc16, 0.9f);
    CHECK(t4.state().locked);
    for (std::int64_t b = 100; b < 150; ++b) {
        t4.onBar(b, arc16(b), 0.9f);
        CHECK(t4.state().phraseStart == (b % 4 == 0));
    }
}

TEST_CASE("FEAT-001: low confidence never flags, never votes, and an existing lock survives without drift") {
    PhraseTracker t;
    REQUIRE(t.setParams(params(8)).ok());
    run(t, 0, 60, arc8, 0.9f);
    REQUIRE(t.state().locked);
    run(t, 60, 76, arc8, 0.2f);                 // gate closed for two phrases
    CHECK_FALSE(t.state().locked);
    CHECK_FALSE(t.state().phraseStart);
    CHECK_FALSE(t.state().preDrop);
    CHECK(t.state().nextPhraseStartBar == -1);
    CHECK(t.state().barsToPhraseStart == 0);
    const auto votesBefore = t.votes();
    run(t, 76, 90, arc8, 0.2f);
    CHECK(t.votes() == votesBefore);            // no votes while gated off
    for (std::int64_t b = 90; b < 100; ++b) {   // gate re-opens, position is still the grid position
        t.onBar(b, arc8(b), 0.9f);
        CHECK(t.state().locked);
        CHECK(t.state().barInPhrase == static_cast<int>(b % 8));
    }
}

TEST_CASE("FEAT-001: NaN confidence counts as closed gate; the lock is not reported from it") {
    PhraseTracker t;
    REQUIRE(t.setParams(params(8)).ok());
    run(t, 0, 60, arc8, 0.9f);
    REQUIRE(t.state().locked);
    t.onBar(60, EnergyPhase::Steady, std::nanf(""));
    CHECK_FALSE(t.state().locked);
}

TEST_CASE("FEAT-001: inconsistent drop positions do not produce a lock") {
    PhraseTracker t;
    REQUIRE(t.setParams(params(8)).ok());
    const std::int64_t dropBars[] = {8, 9, 11, 14, 17, 20, 23, 30};
    for (std::int64_t b = 0; b < 40; ++b) {
        bool drop = false;
        for (std::int64_t d : dropBars) drop = drop || d == b;
        t.onBar(b, drop ? EnergyPhase::Drop : EnergyPhase::Steady, 0.9f);
    }
    CHECK_FALSE(t.state().locked);
    CHECK(t.state().lockShare < 0.6f);
}

TEST_CASE("FEAT-001: nudge moves every boundary by the given bars and wraps; reversing it restores the grid") {
    PhraseTracker t;
    REQUIRE(t.setParams(params(8)).ok());
    run(t, 0, 60, arc8, 0.9f);
    REQUIRE(t.state().locked);
    t.nudge(3);
    CHECK(t.nudgeBars() == 3);
    t.onBar(60, arc8(60), 0.9f);                 // 60 % 8 == 4, nudged position is 1
    CHECK(t.state().barInPhrase == 1);
    CHECK(t.state().phraseStart == false);
    t.onBar(63, arc8(63), 0.9f);                 // 63 % 8 == 7 -> nudged 4
    CHECK(t.state().barInPhrase == 4);
    t.nudge(-3);
    CHECK(t.nudgeBars() == 0);
    t.onBar(64, arc8(64), 0.9f);
    CHECK(t.state().phraseStart);
    t.nudge(-1);                                 // wraps: 15 modulo 16, equivalent to -1 for 8-bar phrases
    CHECK(t.nudgeBars() == 15);
    t.onBar(70, arc8(70), 0.9f);                 // 70 % 8 == 6 (Build); nudged position is 7 = last bar
    CHECK(t.state().barInPhrase == 7);
    CHECK(t.state().preDrop);
    t.onBar(72, arc8(72), 0.9f);                 // nudged position 1
    CHECK(t.state().barInPhrase == 1);
    CHECK_FALSE(t.state().preDrop);
    t.onBar(79, arc8(79), 0.9f);                 // nudged position 0: the phrase starts on bar 79
    CHECK(t.state().phraseStart);
}

TEST_CASE("FEAT-001: markPhraseStart is a manual lock that later drop votes do not move") {
    PhraseTracker t;
    REQUIRE(t.setParams(params(8)).ok());
    t.onBar(0, EnergyPhase::Steady, 0.9f);
    CHECK_FALSE(t.state().locked);
    t.markPhraseStart(5);
    t.onBar(1, EnergyPhase::Steady, 0.9f);
    CHECK(t.state().locked);
    CHECK(t.state().manual);
    CHECK(t.state().barInPhrase == 1 - 5 + 8);
    run(t, 2, 40, arc8, 0.9f);                   // drops at residue 0 do not override the operator tap
    CHECK(t.state().locked);
    CHECK(t.state().barInPhrase == static_cast<int>(((39 - 5) % 8 + 8) % 8));
    t.reset();
    CHECK_FALSE(t.state().locked);
    CHECK_FALSE(t.state().manual);
}

TEST_CASE("FEAT-001: repeated or older bar indexes are ignored and the output is reproducible") {
    auto trace = [] {
        PhraseTracker t;
        (void)t.setParams(params(8));
        std::vector<int> out;
        for (std::int64_t b = 0; b < 80; ++b) {
            t.onBar(b, arc8(b), 0.9f);
            t.onBar(b, arc8(b), 0.9f);           // duplicate: ignored
            out.push_back(t.state().barInPhrase * 4 + (t.state().phraseStart ? 2 : 0) + (t.state().preDrop ? 1 : 0));
        }
        const auto before = t.votes();
        t.onBar(10, EnergyPhase::Drop, 0.9f);    // older bar than the last one: ignored, no extra vote
        CHECK(t.votes() == before);
        return out;
    };
    const auto a = trace();
    const auto b = trace();
    const auto c = trace();
    CHECK(a == b);
    CHECK(b == c);
}
