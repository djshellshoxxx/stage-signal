#include "ss/audio/phrase_tracker.hpp"

#include <cmath>

namespace ss::audio {

namespace {
int wrap(std::int64_t a, int m) {
    const std::int64_t r = a % m;
    return static_cast<int>(r < 0 ? r + m : r);
}
bool allowedLength(int bars) { return bars == 4 || bars == 8 || bars == 16; }
}  // namespace

bool PhraseParams::valid(const char** why) const {
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    if (!allowedLength(phraseBars)) return fail("phraseBars must be 4, 8 or 16");
    if (!std::isfinite(minConfidence) || minConfidence < 0.f || minConfidence > 1.f) return fail("minConfidence outside [0, 1]");
    if (minVotes < 1 || minVotes > 64) return fail("minVotes outside [1, 64]");
    if (!std::isfinite(lockShare) || lockShare < 0.5f || lockShare > 1.f) return fail("lockShare outside [0.5, 1]");
    return true;
}

Status PhraseTracker::setParams(const PhraseParams& p) {
    const char* why = nullptr;
    if (!p.valid(&why)) return makeError(ErrorCategory::InvalidArgument, why ? why : "invalid phrase params");
    p_ = p;
    relockFromVotes();
    publish();
    return okStatus();
}

void PhraseTracker::reset() {
    votes_.fill(0);
    total_ = 0;
    start_ = 0;
    nudge_ = 0;
    voteShare_ = 0.f;
    voteLock_ = manual_ = haveBar_ = gate_ = false;
    curBar_ = 0;
    phase_ = lastPhase_ = EnergyPhase::Steady;
    st_ = PhraseState{};
    st_.phraseBars = p_.phraseBars;
}

void PhraseTracker::onBar(std::int64_t barIndex, EnergyPhase arcPhase, float confidence) {
    if (haveBar_ && barIndex <= curBar_) return;
    gate_ = std::isfinite(confidence) && confidence >= p_.minConfidence;
    // Votes come from Drop entries only, and only while the gate is open. A Drop seen during a gated-off
    // stretch still updates lastPhase_, so it cannot vote later as a fresh entry.
    if (gate_ && arcPhase == EnergyPhase::Drop && lastPhase_ != EnergyPhase::Drop) {
        ++votes_[wrap(barIndex, PhraseLimits::kVoteBars)];
        ++total_;
        relockFromVotes();
    }
    lastPhase_ = arcPhase;
    phase_ = arcPhase;
    curBar_ = barIndex;
    haveBar_ = true;
    publish();
}

void PhraseTracker::markPhraseStart(std::int64_t barIndex) {
    manual_ = true;
    start_ = wrap(barIndex, PhraseLimits::kVoteBars);
    publish();
}

void PhraseTracker::nudge(int bars) {
    nudge_ = wrap(static_cast<std::int64_t>(nudge_) + bars, PhraseLimits::kVoteBars);
    publish();
}

void PhraseTracker::relockFromVotes() {
    const int L = p_.phraseBars;
    std::array<std::uint32_t, PhraseLimits::kVoteBars> score{};
    for (int j = 0; j < PhraseLimits::kVoteBars; ++j) score[static_cast<std::size_t>(j % L)] += votes_[static_cast<std::size_t>(j)];
    int best = 0;
    for (int k = 1; k < L; ++k) {
        if (score[static_cast<std::size_t>(k)] > score[static_cast<std::size_t>(best)]) best = k;
    }
    const float bestCount = static_cast<float>(score[static_cast<std::size_t>(best)]);
    voteShare_ = total_ ? bestCount / static_cast<float>(total_) : 0.f;
    voteLock_ = total_ >= static_cast<std::uint32_t>(p_.minVotes) && voteShare_ >= p_.lockShare;
    if (voteLock_ && !manual_) start_ = best;
}

void PhraseTracker::publish() {
    st_ = PhraseState{};
    st_.phraseBars = p_.phraseBars;
    st_.lockShare = voteShare_;
    st_.manual = manual_;
    if (!haveBar_ || !(manual_ || voteLock_) || !gate_) return;

    const int L = p_.phraseBars;
    const int p = wrap(curBar_ - start_ - nudge_, L);
    st_.locked = true;
    st_.barInPhrase = p;
    st_.phraseStart = p == 0;
    st_.preDrop = p == L - 1 && phase_ == EnergyPhase::Build;
    st_.barsToPhraseStart = L - p;
    st_.nextPhraseStartBar = curBar_ + (L - p);
}

}  // namespace ss::audio
