// StageSignal audio: phrase tracker (FEAT-001). Bar-in-phrase from the beat/bar grid plus the Energy Arc.
// Spec: docs/specs/musical-features.md "FEAT-001". Pure and deterministic: the only inputs are onBar() calls.
#pragma once
#include <array>
#include <cstdint>
#include "ss/audio/energy_arc.hpp"
#include "ss/base/error.hpp"

namespace ss::audio {

struct PhraseLimits {
    static constexpr int kVoteBars = 16;   // residue histogram period; every allowed phrase length divides it
    static constexpr int kMaxNudge = 15;   // nudge is kept modulo kVoteBars
};

struct PhraseParams {
    int phraseBars = 8;            // 4, 8 or 16
    float minConfidence = 0.5f;    // [0, 1] gate on the caller's confidence (use TempoState::confidence)
    int minVotes = 3;              // [1, 64] Drop entries needed before the Energy Arc can lock the phrase
    float lockShare = 0.6f;        // [0.5, 1] share of the votes the best phrase-start residue must hold
    bool valid(const char** why = nullptr) const;
};

struct PhraseState {
    bool locked = false;                     // lock present AND confidence gate open
    int barInPhrase = -1;                    // 0..phraseBars-1 while locked, else -1
    int phraseBars = 8;
    bool phraseStart = false;                // this bar is the first bar of a phrase (while locked)
    bool preDrop = false;                    // this bar is the last bar of a phrase and the arc is in Build
    int barsToPhraseStart = 0;               // bar boundaries until the next phrase start, 1..phraseBars; 0 unlocked
    std::int64_t nextPhraseStartBar = -1;    // absolute bar index of that start; -1 unlocked
    float lockShare = 0.f;                   // share of drop votes behind the vote lock (0..1)
    bool manual = false;                     // lock was set by markPhraseStart()
};

// One instance per show clock. Not thread safe; called from the analysis/show thread. Allocation free.
class PhraseTracker {
public:
    Status setParams(const PhraseParams& p);
    const PhraseParams& params() const { return p_; }
    // Clears votes, lock and nudge; keeps parameters.
    void reset();

    // Call once per bar boundary with the bar index from BeatGrid::barAt(), the Energy Arc phase at that
    // boundary and the confidence in [0, 1]. Repeated or older bar indexes are ignored.
    void onBar(std::int64_t barIndex, EnergyPhase arcPhase, float confidence);

    // Operator tap: the given bar starts a phrase. Sets a manual lock that survives later votes until reset().
    void markPhraseStart(std::int64_t barIndex);
    // Operator nudge: moves all phrase boundaries by `bars` (cumulative, wraps modulo kVoteBars).
    void nudge(int bars);
    int nudgeBars() const { return nudge_; }

    PhraseState state() const { return st_; }
    std::array<std::uint32_t, PhraseLimits::kVoteBars> votes() const { return votes_; }   // diagnostics

private:
    void relockFromVotes();
    void publish();

    PhraseParams p_;
    PhraseState st_;
    std::array<std::uint32_t, PhraseLimits::kVoteBars> votes_{};
    std::uint32_t total_ = 0;
    int start_ = 0;                   // phrase-start residue, bar index modulo kVoteBars
    int nudge_ = 0;
    float voteShare_ = 0.f;
    bool voteLock_ = false, manual_ = false, haveBar_ = false, gate_ = false;
    std::int64_t curBar_ = 0;
    EnergyPhase phase_ = EnergyPhase::Steady, lastPhase_ = EnergyPhase::Steady;
};

}  // namespace ss::audio
