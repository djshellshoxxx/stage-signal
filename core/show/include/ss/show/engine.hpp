// StageSignal show engine (MOD-04): the sole writer of show state. See docs/specs/show.md.
#pragma once
#include <functional>
#include <memory>
#include <vector>
#include "ss/base/actions.hpp"
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/base/types.hpp"
#include "ss/model/show_model.hpp"
#include "ss/show/types.hpp"

namespace ss::show {

// Thread model: every public method takes one internal mutex, so submit()/submitEdit()/loadDocument()
// may be called from UI, MIDI and remote threads while one thread calls tick(). Not for the audio
// callback (it may block briefly). The journal sink is invoked from tick() after the lock is released.
class ShowEngine {
public:
    // `clock` is optional: when set it timestamps submissions (tap tempo, journal wall time).
    // Without it submissions are stamped with the time of the last tick.
    explicit ShowEngine(const IClock* clock = nullptr, Tunables tunables = {});
    ~ShowEngine();
    ShowEngine(const ShowEngine&) = delete;
    ShowEngine& operator=(const ShowEngine&) = delete;

    // Validate completely, then stage; the swap happens at the start of the next tick (safe boundary).
    // On failure the old document stays active and nothing is staged. Blackout latches, the strobe lock and
    // tempo/key overrides survive a load (loading a show never un-blacks or re-enables strobing).
    Status loadDocument(const model::ShowDocument& doc);
    static Status validateDocument(const model::ShowDocument& doc);

    // IF-02. Validates first. Safety-critical ids are applied immediately and never queue or drop.
    ActionResult submit(const Action& a);
    // Editor commands (pool membership, cue list). Undoable.
    ActionResult submitEdit(const EditCommand& e);

    // Operator 'bar' action: declare that `at` is a downbeat. Bars are counted from here.
    void markDownbeat(Ns at);

    // IF-05. Produces the immutable snapshot for `now` (clamped to be monotonic).
    ShowSnapshot tick(Ns now, const FeatureSnapshot& features);
    ShowSnapshot latest() const;                // copy of the last snapshot tick() produced

    model::ShowDocument document() const;       // live working copy (with param/pool/cue edits applied)

    // Journal hooks for diag. With a sink set, entries go to the sink; otherwise they are queued
    // (bounded; overflow drops the newest and is counted in Stats::journalDropped).
    void setJournalSink(std::function<void(const model::JournalEntry&)> sink);
    std::size_t drainJournal(std::vector<model::JournalEntry>& out, std::size_t max = Limits::kJournalCapacity);

    Stats stats() const;
    const Tunables& tunables() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ss::show
