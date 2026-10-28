// StageSignal show engine (MOD-04). Single writer of show state; see docs/specs/show.md for the rules.
#include "ss/show/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "ss/base/rng.hpp"
#include "ss/show/beat_grid.hpp"

namespace ss::show {

namespace {

constexpr std::size_t npos = static_cast<std::size_t>(-1);

struct Queued {
    bool isEdit = false;
    Action a;
    EditCommand e;
    Ns receivedNs = 0;
};

struct Pending {
    Queued q;
    Ns submitNs = 0;
};

struct Route {                 // one per OutputDef
    OutputId id = 0;
    std::size_t rootIdx = 0;
    bool isRoot = false;
};

struct Root {                  // an output that owns scene state
    OutputId id = 0;
    bool has = false;
    std::uint32_t sceneId = 0;
    model::SceneParams look;
    bool hasFrom = false;
    std::uint32_t fromId = 0;
    Ns trStart = 0, trDur = 0;
    model::TransitionKind trKind = model::TransitionKind::Cut;
};

struct ParamPart {
    OutputId root = 0;
    std::uint32_t sceneId = 0;
    model::SceneParams lookBefore, lookAfter, defBefore, defAfter;
};

struct LookPart {
    OutputId root = 0;
    bool hadBefore = false;
    std::uint32_t sceneBefore = 0, sceneAfter = 0;
    model::SceneParams pBefore, pAfter;
};

struct HistEntry {
    enum class Kind : std::uint8_t { Params, Pool, Cue, Mode, Look } kind = Kind::Params;
    std::string label;
    std::uint32_t coalesceKey = 0;
    OutputId coalesceTarget = 0;
    Ns lastNs = 0;
    std::vector<ParamPart> params;
    std::uint32_t poolScene = 0;
    bool poolBefore = false, poolAfter = false;
    EditKind cueKind = EditKind::InsertCue;
    int idx = 0, idx2 = 0;
    model::CueDef cueBefore, cueAfter;
    int cueIdxBefore = -1, cueIdxAfter = -1;
    ShowMode modeBefore = ShowMode::Auto, modeAfter = ShowMode::Auto;
    std::vector<LookPart> looks;
};

bool isInteger(double v) { return std::isfinite(v) && v == std::floor(v); }
Ns secToNsRound(double s) { return static_cast<Ns>(std::llround(s * 1e9)); }

bool handledId(std::uint32_t raw) {
    switch (static_cast<ActionId>(raw)) {
        case ActionId::SceneSelect: case ActionId::NextScene: case ActionId::PrevScene:
        case ActionId::BankNext: case ActionId::BankPrev: case ActionId::Pause: case ActionId::Resume:
        case ActionId::SetMode: case ActionId::NextCue: case ActionId::PrevCue: case ActionId::GoToCue:
        case ActionId::VisualBlackout: case ActionId::LightingBlackout: case ActionId::EmergencyBlackout:
        case ActionId::RestoreBlackout:
        case ActionId::Brightness: case ActionId::Speed: case ActionId::Intensity: case ActionId::ColorHue:
        case ActionId::ColorSaturation:
        case ActionId::SetBpm: case ActionId::TapTempo: case ActionId::ReleaseBpmOverride:
        case ActionId::SetKey: case ActionId::ReleaseKeyOverride:
        case ActionId::DisableStrobing: case ActionId::EnableStrobing:
        case ActionId::EffectPrismMirror: case ActionId::EffectBloomPulse:
        case ActionId::Undo: case ActionId::Redo:
        case ActionId::SnapshotStoreA: case ActionId::SnapshotStoreB:
        case ActionId::SnapshotRecallA: case ActionId::SnapshotRecallB:
        case ActionId::SelectOutput:
            return true;
        default:
            return false;
    }
}

bool isContinuous(ActionId a) {
    return a == ActionId::Brightness || a == ActionId::Speed || a == ActionId::Intensity ||
           a == ActionId::ColorHue || a == ActionId::ColorSaturation || a == ActionId::EffectPrismMirror ||
           a == ActionId::EffectBloomPulse;
}

std::string fmt2(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

}  // namespace

struct ShowEngine::Impl {
    const IClock* clock_;
    Tunables tun_;
    AudioHealthMonitor audioMon_;
    bool audioAvail_ = true;                 // REQ-D09: assumed present until the app reports otherwise
    mutable std::mutex mu_;

    // --- document and derived tables
    model::ShowDocument doc_;
    std::unordered_map<std::uint32_t, std::size_t> sceneIdx_;
    bool stagedValid_ = false;
    model::ShowDocument staged_;
    std::vector<Route> routes_;
    std::vector<Root> roots_;
    Rng rng_{1};

    // --- revision / time
    Revision revision_ = 1;
    bool dirty_ = false;
    bool started_ = false;
    Ns lastTick_ = 0;

    // --- tempo / key / grid
    BeatGrid grid_;
    TempoState tempo_;
    KeyState key_;
    struct TempoOvr { bool active = false; double bpm = 0; ValueSource src = ValueSource::Manual; } tempoOvr_;
    struct KeyOvr { bool active = false; int tonic = -1; bool minor = false; } keyOvr_;
    std::vector<Ns> taps_;
    FeatureSnapshot feat_;
    model::EnergyTier wantedTier_ = model::EnergyTier::Any;

    // --- flow
    ShowMode mode_ = ShowMode::Auto;
    int cueIndex_ = -1;
    Ns cueElapsedNs_ = 0;
    double cueElapsedBeats_ = 0.0;
    bool cueHolding_ = false, cueWaitingTempo_ = false, programEnded_ = false;
    Ns sceneElapsedNs_ = 0;
    bool autoArmed_ = false;
    Ns autoArmedAt_ = 0;
    bool paused_ = false, override_ = false;
    bool visualBo_ = false, lightingBo_ = false, emergencyBo_ = false;
    bool strobeLocked_ = false;
    int bank_ = 0;
    OutputId selectedOutput_ = kAllOutputs;
    LookSlot slots_[Limits::kSnapshotSlots];
    std::deque<std::uint32_t> hist_;     // scene history of the primary root, newest first

    // --- Energy Arc drop
    bool dropArmed_ = false;
    Ns dropDetected_ = 0, dropAt_ = 0, lastDropNs_ = 0;
    bool hasDropped_ = false;
    std::uint8_t lastPhase_ = 0;

    // --- queues
    std::vector<Queued> inbox_;
    std::vector<Pending> pending_;
    std::vector<HistEntry> undo_, redo_;

    // --- events / journal
    std::vector<ShowEvent> events_;
    std::uint32_t eventsDropped_ = 0;
    std::vector<model::JournalEntry> jbuf_;
    std::deque<model::JournalEntry> jq_;
    std::function<void(const model::JournalEntry&)> sink_;
    Stats st_;
    ShowSnapshot last_;

    Impl(const IClock* c, Tunables t) : clock_(c), tun_(t), audioMon_(t.audioHealth), grid_(t.beatsPerBar) {
        installDoc(model::ShowDocument{});
        mode_ = doc_.mode;
        last_ = buildSnapshot(0);
    }

    // ------------------------------------------------------------------ helpers
    Ns submitNow() const { return clock_ ? clock_->nowMonotonic() : lastTick_; }
    bool tempoUsable() const { return tempo_.valid() && grid_.valid(); }
    const model::SceneDef* scene(std::uint32_t id) const {
        auto it = sceneIdx_.find(id);
        return it == sceneIdx_.end() ? nullptr : &doc_.scenes[it->second];
    }
    model::SceneDef* sceneMut(std::uint32_t id) {
        auto it = sceneIdx_.find(id);
        return it == sceneIdx_.end() ? nullptr : &doc_.scenes[it->second];
    }
    int bankCount() const { return std::max<int>(1, static_cast<int>((doc_.scenes.size() + Limits::kScenePadsPerBank - 1) / Limits::kScenePadsPerBank)); }
    std::size_t routeOf(OutputId id) const {
        for (std::size_t i = 0; i < routes_.size(); ++i) if (routes_[i].id == id) return i;
        return npos;
    }
    Ns durNs(double v) const { return secToNsRound(v); }

    void emit(EventKind k, OutputId out, std::uint32_t sceneId, int cue, Ns at, std::string detail) {
        const char* jk = "scene";
        switch (k) {
            case EventKind::SceneChanged: jk = "scene"; break;
            case EventKind::CueFired: case EventKind::CueSkipped: case EventKind::CueListEnded: jk = "cue"; break;
            case EventKind::ModeChanged: case EventKind::OverrideChanged: case EventKind::Paused:
            case EventKind::Resumed: jk = "mode"; break;
            case EventKind::BlackoutChanged: case EventKind::StrobeLockChanged: jk = "safety"; break;
            case EventKind::TempoOverrideChanged: case EventKind::KeyOverrideChanged: jk = "tempo"; break;
            case EventKind::DropScheduled: case EventKind::Drop: case EventKind::DropSkipped: jk = "drop"; break;
            case EventKind::SnapshotStored: case EventKind::SnapshotRecalled: jk = "snapshot"; break;
            case EventKind::UndoApplied: case EventKind::RedoApplied: case EventKind::EditApplied: jk = "edit"; break;
            case EventKind::AudioLost: case EventKind::AudioRestored: jk = "device"; break;
            case EventKind::DocumentLoaded: jk = "doc"; break;
            case EventKind::ActionRejected: jk = "reject"; break;
        }
        model::JournalEntry j;
        j.monoNs = at;
        j.wallUnixMs = clock_ ? clock_->wallUnixNs() / kNsPerMs : 0;
        j.kind = jk;
        j.detail = std::string(eventKindName(k)) + (detail.empty() ? "" : " " + detail);
        if (events_.size() < Limits::kEventsPerTick) {
            ShowEvent ev;
            ev.kind = k; ev.output = out; ev.sceneId = sceneId; ev.cueIndex = cue; ev.atNs = at; ev.detail = std::move(detail);
            events_.push_back(std::move(ev));
        } else {
            ++eventsDropped_; ++st_.eventsDropped;
        }
        jbuf_.push_back(std::move(j));
    }

    // ------------------------------------------------------------------ document install
    void installDoc(model::ShowDocument d) {
        doc_ = std::move(d);
        sceneIdx_.clear();
        for (std::size_t i = 0; i < doc_.scenes.size(); ++i) sceneIdx_[doc_.scenes[i].id] = i;
        routes_.clear(); roots_.clear();
        // primary = first non-Duplicate output. Independent outputs (other than primary) are extra roots.
        std::size_t primary = 0;
        for (std::size_t i = 0; i < doc_.outputs.size(); ++i)
            if (doc_.outputs[i].kind != RouteKind::Duplicate) { primary = i; break; }
        Root pr; pr.id = doc_.outputs[primary].id; roots_.push_back(pr);
        for (std::size_t i = 0; i < doc_.outputs.size(); ++i) {
            if (i != primary && doc_.outputs[i].kind == RouteKind::Independent) { Root r; r.id = doc_.outputs[i].id; roots_.push_back(r); }
        }
        auto rootIdxOfId = [&](OutputId id) -> std::size_t {
            for (std::size_t i = 0; i < roots_.size(); ++i) if (roots_[i].id == id) return i;
            return 0;
        };
        for (const auto& o : doc_.outputs) {
            // resolve duplicates to their source
            const model::OutputDef* cur = &o;
            for (std::size_t hop = 0; hop <= doc_.outputs.size() && cur->kind == RouteKind::Duplicate; ++hop) {
                const model::OutputDef* nx = nullptr;
                for (const auto& p : doc_.outputs) if (p.id == cur->duplicateOf) { nx = &p; break; }
                if (!nx) break;
                cur = nx;
            }
            Route r; r.id = o.id;
            r.rootIdx = (cur->kind == RouteKind::Independent) ? rootIdxOfId(cur->id) : 0;
            r.isRoot = (r.rootIdx < roots_.size() && roots_[r.rootIdx].id == o.id);
            routes_.push_back(r);
        }
    }

    void installStaged(Ns now) {
        const std::size_t discarded = inbox_.size() + pending_.size();
        install(std::move(staged_), now);
        stagedValid_ = false;
        staged_ = model::ShowDocument{};
        if (discarded) emit(EventKind::ActionRejected, kAllOutputs, 0, -1, now, "queued actions discarded by document load");
    }
    void install(model::ShowDocument d, Ns now) {
        installDoc(std::move(d));
        mode_ = doc_.mode;
        rng_.reseed(doc_.seed);
        cueIndex_ = -1; cueElapsedNs_ = 0; cueElapsedBeats_ = 0; cueHolding_ = false; programEnded_ = false;
        sceneElapsedNs_ = 0; autoArmed_ = false; dropArmed_ = false;
        paused_ = false; override_ = false;
        hist_.clear(); undo_.clear(); redo_.clear();
        for (auto& s : slots_) s = LookSlot{};
        bank_ = 0; selectedOutput_ = kAllOutputs;
        strobeLocked_ = strobeLocked_ || doc_.strobingDisabled;   // loading never re-enables strobing
        inbox_.clear(); pending_.clear();
        ++revision_; dirty_ = false;
        emit(EventKind::DocumentLoaded, kAllOutputs, 0, -1, now,
             "scenes=" + std::to_string(doc_.scenes.size()) + " cues=" + std::to_string(doc_.cues.size()));
    }

    // ------------------------------------------------------------------ targets
    // Fills `out` with root indices. Returns false for an unknown output id.
    bool targetRoots(OutputId t, std::vector<std::size_t>& out) const {
        out.clear();
        if (t == kAllOutputs) { for (std::size_t i = 0; i < roots_.size(); ++i) out.push_back(i); return true; }
        const std::size_t r = routeOf(t);
        if (r == npos) return false;
        out.push_back(routes_[r].rootIdx);
        return true;
    }

    // ------------------------------------------------------------------ scene change
    void changeScene(std::size_t ri, std::uint32_t sceneId, model::TransitionKind tk, double sec, Ns now, const char* cause) {
        Root& r = roots_[ri];
        const model::SceneDef* sd = scene(sceneId);
        if (!sd) return;
        const bool had = r.has;
        r.hasFrom = had; r.fromId = r.sceneId;
        r.trKind = tk; r.trStart = now;
        r.trDur = (tk == model::TransitionKind::Cut || sec <= 0.0) ? 0 : durNs(sec);
        if (r.trDur == 0) r.hasFrom = false;
        r.has = true; r.sceneId = sceneId; r.look = sd->params;
        if (ri == 0) {
            sceneElapsedNs_ = 0; autoArmed_ = false;
            hist_.push_front(sceneId);
            while (hist_.size() > Limits::kRepeatHistoryMax) hist_.pop_back();
        }
        dirty_ = true;
        emit(EventKind::SceneChanged, r.id, sceneId, ri == 0 ? cueIndex_ : -1, now, std::string("cause=") + cause);
    }

    int safeSceneIndex() const {
        int firstAny = -1;
        for (std::size_t i = 0; i < doc_.scenes.size(); ++i) {
            if (doc_.scenes[i].strobe) continue;
            if (doc_.scenes[i].inAutoPool) return static_cast<int>(i);
            if (firstAny < 0) firstAny = static_cast<int>(i);
        }
        return firstAny;
    }

    // Next/previous pad scene for a root, skipping locked strobe scenes. -1 = none other than current.
    int stepScene(const Root& r, int dir) const {
        const int n = static_cast<int>(doc_.scenes.size());
        if (n == 0) return -1;
        int cur = -1;
        if (r.has) { auto it = sceneIdx_.find(r.sceneId); if (it != sceneIdx_.end()) cur = static_cast<int>(it->second); }
        int i = cur < 0 ? (dir > 0 ? -1 : n) : cur;
        for (int step = 0; step < n; ++step) {
            i = (i + dir + n) % n;
            if (i == cur) break;
            if (strobeLocked_ && doc_.scenes[static_cast<std::size_t>(i)].strobe) continue;
            return i;
        }
        return -1;
    }

    // ------------------------------------------------------------------ override / mode
    void engageOverride(Ns now, const char* cause) {
        dropArmed_ = false;
        if (override_) return;
        override_ = true; dirty_ = true;
        if (mode_ == ShowMode::Auto) {
            mode_ = ShowMode::Hybrid;
            emit(EventKind::ModeChanged, kAllOutputs, 0, -1, now, "Auto->Hybrid cause=manual");
        }
        emit(EventKind::OverrideChanged, kAllOutputs, 0, -1, now, std::string("on cause=") + cause);
    }

    void setModeInternal(ShowMode m, Ns now, const char* cause) {
        if (m == mode_) return;
        static const char* names[] = {"Auto", "Programmed", "Hybrid"};
        const std::string d = std::string(names[static_cast<int>(mode_)]) + "->" + names[static_cast<int>(m)] + " cause=" + cause;
        mode_ = m;
        dropArmed_ = false; autoArmed_ = false;
        if (override_) { override_ = false; emit(EventKind::OverrideChanged, kAllOutputs, 0, -1, now, "off cause=mode"); }
        if (m == ShowMode::Programmed && cueIndex_ >= 0 && cueIndex_ < static_cast<int>(doc_.cues.size())) {
            cueElapsedNs_ = 0; cueElapsedBeats_ = 0; cueHolding_ = false;   // re-enter the current cue without re-firing it
        }
        dirty_ = true;
        emit(EventKind::ModeChanged, kAllOutputs, 0, -1, now, d);
    }

    // ------------------------------------------------------------------ cues
    bool cueSkipped(std::size_t i) const {
        if (!strobeLocked_) return false;
        const model::SceneDef* s = scene(doc_.cues[i].sceneId);
        return s && s->strobe;
    }
    int nextEligibleCue(int from, bool wrap, bool emitSkips, Ns now) {
        const int n = static_cast<int>(doc_.cues.size());
        for (int i = from + 1; i < n; ++i) {
            if (!cueSkipped(static_cast<std::size_t>(i))) return i;
            if (emitSkips) emit(EventKind::CueSkipped, roots_[0].id, doc_.cues[static_cast<std::size_t>(i)].sceneId, i, now, "strobing disabled");
        }
        if (wrap) {
            for (int i = 0; i <= from && i < n; ++i) if (!cueSkipped(static_cast<std::size_t>(i))) return i;
        }
        return -1;
    }
    int prevEligibleCue(int from) const {
        for (int i = from - 1; i >= 0; --i) if (!cueSkipped(static_cast<std::size_t>(i))) return i;
        return -1;
    }
    void enterCue(int idx, Ns now) {
        const model::CueDef& c = doc_.cues[static_cast<std::size_t>(idx)];
        cueIndex_ = idx; cueElapsedNs_ = 0; cueElapsedBeats_ = 0; cueHolding_ = false; programEnded_ = false;
        changeScene(0, c.sceneId, c.transition, c.transitionDuration, now, "cue");
        dirty_ = true;
        emit(EventKind::CueFired, roots_[0].id, c.sceneId, idx, now, "");
    }

    void programmedStep(Ns now, Ns dt) {
        if (doc_.cues.empty()) return;
        if (cueIndex_ < 0) {
            if (programEnded_) return;
            const int first = nextEligibleCue(-1, false, true, now);
            if (first < 0) { programEnded_ = true; dirty_ = true; emit(EventKind::CueListEnded, roots_[0].id, 0, -1, now, "no eligible cue"); return; }
            if (doc_.cues[static_cast<std::size_t>(first)].waitForTrigger) { if (!cueHolding_) { cueHolding_ = true; dirty_ = true; } return; }
            enterCue(first, now);
            return;
        }
        if (cueIndex_ >= static_cast<int>(doc_.cues.size())) return;
        const model::CueDef& c = doc_.cues[static_cast<std::size_t>(cueIndex_)];
        cueElapsedNs_ += dt;
        cueWaitingTempo_ = false;
        if (c.timeBase == model::TimeBase::Beats) {
            if (tempoUsable()) cueElapsedBeats_ += static_cast<double>(dt) / static_cast<double>(beatNs(tempo_.bpm));
            else cueWaitingTempo_ = true;
        }
        if (c.duration <= 0.0) { cueHolding_ = true; return; }
        const bool done = (c.timeBase == model::TimeBase::Seconds) ? cueElapsedNs_ >= durNs(c.duration)
                                                                    : cueElapsedBeats_ + 1e-6 >= c.duration;
        if (!done || programEnded_) return;
        int nxt = nextEligibleCue(cueIndex_, false, true, now);
        if (nxt < 0 && tun_.loopCues) nxt = nextEligibleCue(-1, false, true, now);
        if (nxt < 0) {
            programEnded_ = true; cueHolding_ = true; dirty_ = true;
            emit(EventKind::CueListEnded, roots_[0].id, c.sceneId, cueIndex_, now, "");
            return;
        }
        if (doc_.cues[static_cast<std::size_t>(nxt)].waitForTrigger) {
            if (!cueHolding_) { cueHolding_ = true; dirty_ = true; }
            return;
        }
        enterCue(nxt, now);
    }

    // ------------------------------------------------------------------ auto pool
    std::uint32_t tierWeight(const model::SceneDef& s) const {
        using T = model::EnergyTier;
        if (tun_.reserveDropScenes && s.tier == T::Drop) return 0;
        if (!tun_.energyArcEnabled || wantedTier_ == T::Any || s.tier == T::Any) return tun_.tierAnyWeight;
        auto rank = [](T t) { return t == T::Low ? 1 : t == T::Medium ? 2 : 3; };   // Drop ranks as High
        const int diff = std::abs(rank(s.tier) - rank(wantedTier_));
        return diff == 0 ? tun_.tierMatchWeight : diff == 1 ? tun_.tierAdjacentWeight : tun_.tierFarWeight;
    }

    // Candidate ids honouring repeat history; relaxes the history window (never the "not current" rule).
    void candidates(const std::vector<std::uint32_t>& pool, std::vector<std::uint32_t>& out) const {
        out.clear();
        const bool hasCur = roots_[0].has;
        const std::uint32_t cur = roots_[0].sceneId;
        const int h = std::min<int>(doc_.repeatHistory, static_cast<int>(hist_.size()));
        for (int k = h; k >= 0; --k) {
            out.clear();
            for (std::uint32_t id : pool) {
                if (hasCur && id == cur) continue;
                bool inHist = false;
                for (int j = 0; j < k; ++j) if (hist_[static_cast<std::size_t>(j)] == id) { inHist = true; break; }
                if (!inHist) out.push_back(id);
            }
            if (!out.empty()) return;
        }
    }

    bool autoPick(Ns now, const char* cause) {
        std::vector<std::uint32_t> pool, cands;
        for (const auto& s : doc_.scenes) if (s.inAutoPool && !(strobeLocked_ && s.strobe)) pool.push_back(s.id);
        if (pool.empty()) return false;
        candidates(pool, cands);
        if (cands.empty()) return false;          // pool of one: nothing legal to change to
        std::vector<std::uint32_t> w(cands.size());
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < cands.size(); ++i) { w[i] = tierWeight(*scene(cands[i])); total += w[i]; }
        if (total == 0) { for (auto& x : w) x = 100; total = 100ull * w.size(); }
        std::uint64_t r = rng_.below(static_cast<std::uint32_t>(total));
        std::size_t pick = 0;
        for (; pick + 1 < cands.size(); ++pick) { if (r < w[pick]) break; r -= w[pick]; }
        changeScene(0, cands[pick], tun_.autoTransition, tun_.autoTransitionSec, now, cause);
        return true;
    }

    bool dropPick(Ns now) {
        std::vector<std::uint32_t> pool, cands;
        for (const auto& s : doc_.scenes)
            if (s.inAutoPool && s.tier == model::EnergyTier::Drop && !(strobeLocked_ && s.strobe)) pool.push_back(s.id);
        if (pool.empty()) return false;
        candidates(pool, cands);
        if (cands.empty()) return false;
        const std::uint32_t id = cands[rng_.below(static_cast<std::uint32_t>(cands.size()))];
        changeScene(0, id, tun_.autoTransition, tun_.autoTransitionSec, now, "drop");
        return true;
    }

    Ns minDwellNs() const { return static_cast<Ns>(doc_.minDwellSec) * kNsPerSec; }

    void autoStep(Ns now, Ns dt) {
        if (doc_.scenes.empty()) return;
        if (!roots_[0].has) { autoPick(now, "initial"); return; }
        sceneElapsedNs_ += dt;
        const Ns interval = std::max(minDwellNs(), durNs(tun_.autoIntervalSec));
        if (!autoArmed_ && sceneElapsedNs_ >= interval) {
            if (tun_.quantiseAutoToBar && tempoUsable()) { autoArmed_ = true; autoArmedAt_ = now; }
            else {
                if (!autoPick(now, "auto")) sceneElapsedNs_ = 0;      // nothing legal: re-arm, no spin
                return;
            }
        }
        if (autoArmed_) {
            const bool due = tempoUsable() ? now >= grid_.boundaryAfter(autoArmedAt_, true)
                                           : (now - autoArmedAt_) >= tun_.quantiseFallbackNs;
            if (due) {
                autoArmed_ = false;
                if (!autoPick(now, "auto")) sceneElapsedNs_ = 0;      // nothing legal: re-arm, no spin
            }
        }
    }

    void dropStep(Ns now) {
        const std::uint8_t phase = feat_.arcPhase;
        const bool edge = phase == 2 && lastPhase_ != 2;
        lastPhase_ = phase;
        if (!tun_.energyArcEnabled || !tun_.dropEnabled) return;
        const bool running = !paused_ && !override_ && mode_ != ShowMode::Programmed;
        if (!running) {
            if (dropArmed_) { dropArmed_ = false; dirty_ = true; emit(EventKind::DropSkipped, roots_[0].id, 0, -1, now, "automation held"); }
            return;
        }
        if (edge && !dropArmed_) {
            const bool cooled = !hasDropped_ || (now - lastDropNs_) >= durNs(tun_.dropCooldownSec);
            if (cooled) {
                dropArmed_ = true; dropDetected_ = now; dropAt_ = now; dirty_ = true;
                emit(EventKind::DropScheduled, roots_[0].id, 0, -1, now, tempoUsable() ? "quantise=bar" : "quantise=none");
            }
        }
        if (!dropArmed_) return;
        if (now - dropDetected_ > durNs(tun_.dropExpireSec)) {
            dropArmed_ = false; dirty_ = true;
            emit(EventKind::DropSkipped, roots_[0].id, 0, -1, now, "expired");
            return;
        }
        const Ns boundary = tempoUsable() ? grid_.boundaryAfter(dropAt_, true) : now;
        if (now < boundary) return;
        if (sceneElapsedNs_ < minDwellNs()) {                 // dwell not satisfied: try the next bar
            if (tempoUsable()) dropAt_ = boundary;
            return;
        }
        dropArmed_ = false; dirty_ = true;
        if (dropPick(now)) {
            hasDropped_ = true; lastDropNs_ = now;
            emit(EventKind::Drop, roots_[0].id, roots_[0].sceneId, -1, now, tempoUsable() ? "quantised=bar" : "unquantised");
        } else {
            emit(EventKind::DropSkipped, roots_[0].id, 0, -1, now, "no eligible drop-tier scene");
        }
    }

    // ------------------------------------------------------------------ validation
    std::string validateAction(const Action& a, bool submitPhase) const {
        if (!handledId(a.actionId)) return "action not handled by show engine";
        const ActionId id_ = static_cast<ActionId>(a.actionId);
        const bool crit = isSafetyCritical(id_);
        if (!crit) {
            if (submitPhase && a.expectedRevision != 0 && a.expectedRevision != revision_) return "stale revision";
            switch (a.trigger) {
                case TriggerKind::Immediate: break;
                case TriggerKind::NextBeat: case TriggerKind::NextBar:
                    if (submitPhase && !tempoUsable()) return "no tempo available for quantised trigger";
                    break;
                case TriggerKind::AtTime: if (a.triggerTime < 0) return "invalid trigger time"; break;
                default: return "invalid trigger kind";
            }
        }
        std::vector<std::size_t> tr;
        const bool needTarget = id_ == ActionId::SceneSelect || id_ == ActionId::NextScene || id_ == ActionId::PrevScene ||
                                isContinuous(id_) || id_ == ActionId::SnapshotStoreA || id_ == ActionId::SnapshotStoreB ||
                                id_ == ActionId::SnapshotRecallA || id_ == ActionId::SnapshotRecallB;
        if (needTarget && !targetRoots(a.target, tr)) return "unknown output";
        switch (id_) {
            case ActionId::SceneSelect: {
                const model::SceneDef* s = nullptr;
                if (a.fixtureId != 0) s = scene(a.fixtureId);
                else {
                    if (!isInteger(a.value) || a.value < 0 || a.value >= static_cast<double>(Limits::kScenePadsPerBank)) return "scene slot out of range";
                    const std::size_t i = static_cast<std::size_t>(bank_) * Limits::kScenePadsPerBank + static_cast<std::size_t>(a.value);
                    if (i < doc_.scenes.size()) s = &doc_.scenes[i];
                }
                if (!s) return "no scene in that slot";
                if (strobeLocked_ && s->strobe) return "strobing disabled";
                return {};
            }
            case ActionId::NextScene: case ActionId::PrevScene: {
                if (doc_.scenes.empty()) return "show has no scenes";
                bool any = false;
                for (std::size_t ri : tr) any = any || stepScene(roots_[ri], id_ == ActionId::NextScene ? 1 : -1) >= 0;
                return any ? std::string() : "no other eligible scene";
            }
            case ActionId::Resume:
                if (a.value != 0.0 && a.value != 1.0) return "resume value must be 0 (continue) or 1 (advance)";
                if (a.value == 1.0 && mode_ == ShowMode::Programmed) {
                    if (nextEligibleCueConst(cueIndex_, tun_.loopCues) < 0) return "no next cue";
                }
                return {};
            case ActionId::SetMode:
                if (!isInteger(a.value) || a.value < 0 || a.value > 2) return "invalid mode";
                return {};
            case ActionId::NextCue: case ActionId::PrevCue: case ActionId::GoToCue: {
                if (mode_ != ShowMode::Programmed) return "cue actions require Programmed mode";
                if (doc_.cues.empty()) return "show has no cues";
                if (id_ == ActionId::NextCue) return nextEligibleCueConst(cueIndex_, tun_.loopCues) < 0 ? "no next cue" : std::string();
                if (id_ == ActionId::PrevCue) return (cueIndex_ < 0 || prevEligibleCue(cueIndex_) < 0) ? "no previous cue" : std::string();
                if (!isInteger(a.value) || a.value < 0 || a.value >= static_cast<double>(doc_.cues.size())) return "cue index out of range";
                if (cueSkipped(static_cast<std::size_t>(a.value))) return "strobing disabled";
                return {};
            }
            case ActionId::Brightness: case ActionId::Speed: case ActionId::Intensity: case ActionId::ColorHue:
            case ActionId::ColorSaturation: case ActionId::EffectPrismMirror: case ActionId::EffectBloomPulse: {
                if (!std::isfinite(a.value) || a.value < 0.0 || a.value > 1.0) return "value out of range";
                if (id_ == ActionId::EffectBloomPulse && strobeLocked_ && a.value > tun_.strobeLockedBloomMax) return "strobing disabled";
                bool any = false;
                for (std::size_t ri : tr) any = any || roots_[ri].has;
                return any ? std::string() : "no active scene";
            }
            case ActionId::SetBpm:
                if (!std::isfinite(a.value) || a.value < tun_.minBpm || a.value > tun_.maxBpm) return "bpm out of range";
                return {};
            case ActionId::SetKey:
                if (!isInteger(a.value) || a.value < 0 || a.value > 23) return "key out of range";
                return {};
            case ActionId::Undo: return undo_.empty() ? "nothing to undo" : histBlocked(undo_.back(), true);
            case ActionId::Redo: return redo_.empty() ? "nothing to redo" : histBlocked(redo_.back(), false);
            case ActionId::SnapshotStoreA: case ActionId::SnapshotStoreB: {
                bool any = false;
                for (std::size_t ri : tr) any = any || roots_[ri].has;
                if (a.target == kAllOutputs) return roots_[0].has ? std::string() : "no active scene";
                return any ? std::string() : "no active scene";
            }
            case ActionId::SnapshotRecallA: case ActionId::SnapshotRecallB: {
                const LookSlot& s = slots_[id_ == ActionId::SnapshotRecallA ? 0 : 1];
                if (!s.used) return "snapshot slot is empty";
                const model::SceneDef* sd = scene(s.sceneId);
                if (!sd) return "snapshot scene no longer exists";
                if (strobeLocked_ && sd->strobe) return "strobing disabled";
                return {};
            }
            case ActionId::SelectOutput:
                if (a.value == -1.0) return {};
                if (!isInteger(a.value) || a.value < 0 || routeOf(static_cast<OutputId>(a.value)) == npos) return "unknown output";
                return {};
            default: return {};
        }
    }

    int nextEligibleCueConst(int from, bool wrap) const {
        const int n = static_cast<int>(doc_.cues.size());
        for (int i = from + 1; i < n; ++i) if (!cueSkipped(static_cast<std::size_t>(i))) return i;
        if (wrap) for (int i = 0; i <= from && i < n; ++i) if (!cueSkipped(static_cast<std::size_t>(i))) return i;
        return -1;
    }

    std::string histBlocked(const HistEntry& h, bool isUndo) const {
        if (h.kind == HistEntry::Kind::Look && strobeLocked_) {
            for (const auto& p : h.looks) {
                const std::uint32_t sid = isUndo ? p.sceneBefore : p.sceneAfter;
                const model::SceneDef* s = scene(sid);
                if ((isUndo ? p.hadBefore : true) && s && s->strobe) return "strobing disabled";
            }
        }
        return {};
    }

    std::string validateEdit(const EditCommand& e, bool submitPhase) const {
        if (submitPhase && e.expectedRevision != 0 && e.expectedRevision != revision_) return "stale revision";
        const int n = static_cast<int>(doc_.cues.size());
        auto cueOk = [&](const model::CueDef& c, int ignoreIdx) -> std::string {
            if (!scene(c.sceneId)) return "cue references a missing scene";
            if (!std::isfinite(c.duration) || c.duration < 0) return "cue duration out of range";
            if (!std::isfinite(c.transitionDuration) || c.transitionDuration < 0 || c.transitionDuration > 60.0) return "transition duration out of range";
            if (static_cast<int>(c.transition) > 2) return "invalid transition";
            for (int i = 0; i < n; ++i) if (i != ignoreIdx && doc_.cues[static_cast<std::size_t>(i)].id == c.id) return "duplicate cue id";
            return {};
        };
        switch (e.kind) {
            case EditKind::SetPoolMembership: return scene(e.sceneId) ? std::string() : "unknown scene";
            case EditKind::InsertCue:
                if (e.index < 0 || e.index > n) return "cue index out of range";
                if (doc_.cues.size() >= model::Limits::kMaxCues) return "too many cues";
                return cueOk(e.cue, -1);
            case EditKind::RemoveCue: return (e.index < 0 || e.index >= n) ? "cue index out of range" : std::string();
            case EditKind::ReplaceCue:
                if (e.index < 0 || e.index >= n) return "cue index out of range";
                return cueOk(e.cue, e.index);
            case EditKind::MoveCue:
                return (e.index < 0 || e.index >= n || e.toIndex < 0 || e.toIndex >= n) ? "cue index out of range" : std::string();
        }
        return "unknown edit";
    }

    // ------------------------------------------------------------------ history
    void pushHistory(HistEntry h) {
        redo_.clear();
        undo_.push_back(std::move(h));
        if (undo_.size() > Limits::kHistoryDepth) undo_.erase(undo_.begin());
        dirty_ = true;
    }

    static void adjustCueIndex(int& ci, EditKind k, int idx, int idx2, int sizeAfter) {
        switch (k) {
            case EditKind::InsertCue: if (ci >= 0 && idx <= ci) ++ci; break;
            case EditKind::RemoveCue:
                if (ci >= 0) { if (idx < ci) --ci; else if (idx == ci && ci >= sizeAfter) ci = sizeAfter - 1; }
                break;
            case EditKind::MoveCue:
                if (ci >= 0) {
                    if (ci == idx) ci = idx2;
                    else if (idx < ci && ci <= idx2) --ci;
                    else if (idx2 <= ci && ci < idx) ++ci;
                }
                break;
            default: break;
        }
        if (sizeAfter == 0) ci = -1;
    }

    // Forward cue operation; fills h.cueBefore for Remove/Replace.
    void cueForward(HistEntry& h) {
        auto& v = doc_.cues;
        switch (h.cueKind) {
            case EditKind::InsertCue: v.insert(v.begin() + h.idx, h.cueAfter); break;
            case EditKind::RemoveCue: h.cueBefore = v[static_cast<std::size_t>(h.idx)]; v.erase(v.begin() + h.idx); break;
            case EditKind::ReplaceCue: h.cueBefore = v[static_cast<std::size_t>(h.idx)]; v[static_cast<std::size_t>(h.idx)] = h.cueAfter; break;
            case EditKind::MoveCue: { auto c = v[static_cast<std::size_t>(h.idx)]; v.erase(v.begin() + h.idx); v.insert(v.begin() + h.idx2, c); break; }
            default: break;
        }
    }
    void cueInverse(const HistEntry& h) {
        auto& v = doc_.cues;
        switch (h.cueKind) {
            case EditKind::InsertCue: v.erase(v.begin() + h.idx); break;
            case EditKind::RemoveCue: v.insert(v.begin() + h.idx, h.cueBefore); break;
            case EditKind::ReplaceCue: v[static_cast<std::size_t>(h.idx)] = h.cueBefore; break;
            case EditKind::MoveCue: { auto c = v[static_cast<std::size_t>(h.idx2)]; v.erase(v.begin() + h.idx2); v.insert(v.begin() + h.idx, c); break; }
            default: break;
        }
    }

    std::size_t rootById(OutputId id) const {
        for (std::size_t i = 0; i < roots_.size(); ++i) if (roots_[i].id == id) return i;
        return npos;
    }

    void applyHist(HistEntry& h, bool isUndo, Ns now) {
        switch (h.kind) {
            case HistEntry::Kind::Params: {
                auto doPart = [&](const ParamPart& p) {
                    const std::size_t ri = rootById(p.root);
                    if (model::SceneDef* sd = sceneMut(p.sceneId)) sd->params = isUndo ? p.defBefore : p.defAfter;
                    if (ri != npos && roots_[ri].has && roots_[ri].sceneId == p.sceneId) roots_[ri].look = isUndo ? p.lookBefore : p.lookAfter;
                };
                if (isUndo) for (auto it = h.params.rbegin(); it != h.params.rend(); ++it) doPart(*it);
                else for (const auto& p : h.params) doPart(p);
                break;
            }
            case HistEntry::Kind::Pool:
                if (model::SceneDef* sd = sceneMut(h.poolScene)) sd->inAutoPool = isUndo ? h.poolBefore : h.poolAfter;
                break;
            case HistEntry::Kind::Cue:
                if (isUndo) { cueInverse(h); cueIndex_ = h.cueIdxBefore; } else { cueForward(h); cueIndex_ = h.cueIdxAfter; }
                if (cueIndex_ >= static_cast<int>(doc_.cues.size())) cueIndex_ = static_cast<int>(doc_.cues.size()) - 1;
                break;
            case HistEntry::Kind::Mode:
                setModeInternal(isUndo ? h.modeBefore : h.modeAfter, now, isUndo ? "undo" : "redo");
                break;
            case HistEntry::Kind::Look:
                for (const auto& p : h.looks) {
                    const std::size_t ri = rootById(p.root);
                    if (ri == npos) continue;
                    if (isUndo && !p.hadBefore) continue;
                    changeScene(ri, isUndo ? p.sceneBefore : p.sceneAfter, tun_.manualTransition, tun_.manualTransitionSec, now, isUndo ? "undo" : "redo");
                    roots_[ri].look = isUndo ? p.pBefore : p.pAfter;
                }
                break;
        }
        dirty_ = true;
    }

    // ------------------------------------------------------------------ apply
    void applyEdit(const EditCommand& e, Ns now) {
        const std::string why = validateEdit(e, false);
        if (!why.empty()) { emit(EventKind::ActionRejected, kAllOutputs, 0, -1, now, "edit: " + why); return; }
        HistEntry h;
        h.kind = e.kind == EditKind::SetPoolMembership ? HistEntry::Kind::Pool : HistEntry::Kind::Cue;
        h.cueKind = e.kind; h.idx = e.index; h.idx2 = e.toIndex; h.cueAfter = e.cue;
        h.lastNs = now;
        if (e.kind == EditKind::SetPoolMembership) {
            model::SceneDef* sd = sceneMut(e.sceneId);
            if (sd->inAutoPool == e.inPool) return;                // no-op: no history entry
            h.poolScene = e.sceneId; h.poolBefore = sd->inAutoPool; h.poolAfter = e.inPool; h.label = "pool";
            sd->inAutoPool = e.inPool;
        } else {
            h.label = "cues";
            h.cueIdxBefore = cueIndex_;
            cueForward(h);
            int ci = cueIndex_;
            adjustCueIndex(ci, e.kind, e.index, e.toIndex, static_cast<int>(doc_.cues.size()));
            cueIndex_ = ci; h.cueIdxAfter = ci;
        }
        pushHistory(std::move(h));
        emit(EventKind::EditApplied, kAllOutputs, e.sceneId, e.index, now, e.kind == EditKind::SetPoolMembership ? "pool" : "cues");
    }

    void applyParam(const Action& a, Ns now) {
        const ActionId id_ = static_cast<ActionId>(a.actionId);
        std::vector<std::size_t> tr;
        targetRoots(a.target, tr);
        const float v = static_cast<float>(a.value);
        auto set = [&](model::SceneParams& p) {
            switch (id_) {
                case ActionId::Brightness: p.brightness = v; break;
                case ActionId::Speed: p.speed = v; break;
                case ActionId::Intensity: p.intensity = v; break;
                case ActionId::ColorHue: p.hue = v; break;
                case ActionId::ColorSaturation: p.saturation = v; break;
                case ActionId::EffectPrismMirror: p.effectAmount[0] = v; break;
                case ActionId::EffectBloomPulse: p.effectAmount[1] = v; break;
                default: break;
            }
        };
        HistEntry h;
        h.kind = HistEntry::Kind::Params; h.label = "params"; h.coalesceKey = a.actionId; h.coalesceTarget = a.target; h.lastNs = now;
        for (std::size_t ri : tr) {
            Root& r = roots_[ri];
            if (!r.has) continue;
            model::SceneDef* sd = sceneMut(r.sceneId);
            ParamPart p; p.root = r.id; p.sceneId = r.sceneId; p.lookBefore = r.look; p.defBefore = sd->params;
            set(r.look); set(sd->params);
            p.lookAfter = r.look; p.defAfter = sd->params;
            if (p.lookBefore == p.lookAfter && p.defBefore == p.defAfter) continue;
            h.params.push_back(p);
        }
        if (h.params.empty()) return;
        dirty_ = true;
        if (!undo_.empty()) {
            HistEntry& top = undo_.back();
            if (top.kind == HistEntry::Kind::Params && top.coalesceKey == h.coalesceKey && top.coalesceTarget == h.coalesceTarget &&
                now - top.lastNs <= tun_.coalesceNs && top.params.size() == h.params.size()) {
                bool same = true;
                for (std::size_t i = 0; i < top.params.size(); ++i)
                    same = same && top.params[i].root == h.params[i].root && top.params[i].sceneId == h.params[i].sceneId;
                if (same) {
                    for (std::size_t i = 0; i < top.params.size(); ++i) {
                        top.params[i].lookAfter = h.params[i].lookAfter;
                        top.params[i].defAfter = h.params[i].defAfter;
                    }
                    top.lastNs = now;
                    redo_.clear();
                    return;
                }
            }
        }
        pushHistory(std::move(h));
    }

    void applyAction(const Action& a, Ns now, Ns receivedNs, bool critical) {
        if (!critical) {
            const std::string why = validateAction(a, false);
            if (!why.empty()) { emit(EventKind::ActionRejected, kAllOutputs, 0, -1, now, "action " + std::to_string(a.actionId) + ": " + why); return; }
        }
        const ActionId id_ = static_cast<ActionId>(a.actionId);
        std::vector<std::size_t> tr;
        switch (id_) {
            // ---- scene pads (live: not undoable)
            case ActionId::SceneSelect: {
                targetRoots(a.target, tr);
                std::uint32_t sid;
                if (a.fixtureId != 0) sid = a.fixtureId;
                else sid = doc_.scenes[static_cast<std::size_t>(bank_) * Limits::kScenePadsPerBank + static_cast<std::size_t>(a.value)].id;
                bool prim = false;
                for (std::size_t ri : tr) { changeScene(ri, sid, tun_.manualTransition, tun_.manualTransitionSec, now, "manual"); prim = prim || ri == 0; }
                if (prim) engageOverride(now, "scene");
                break;
            }
            case ActionId::NextScene: case ActionId::PrevScene: {
                targetRoots(a.target, tr);
                bool prim = false;
                for (std::size_t ri : tr) {
                    const int i = stepScene(roots_[ri], id_ == ActionId::NextScene ? 1 : -1);
                    if (i < 0) continue;
                    changeScene(ri, doc_.scenes[static_cast<std::size_t>(i)].id, tun_.manualTransition, tun_.manualTransitionSec, now, "manual");
                    prim = prim || ri == 0;
                }
                if (prim) engageOverride(now, "scene");
                break;
            }
            case ActionId::BankNext: case ActionId::BankPrev: {
                const int bc = bankCount();
                bank_ = (bank_ + (id_ == ActionId::BankNext ? 1 : -1) + bc) % bc;
                dirty_ = true;
                break;
            }
            // ---- flow
            case ActionId::Pause:
                if (!paused_) { paused_ = true; dirty_ = true; emit(EventKind::Paused, kAllOutputs, 0, cueIndex_, now, ""); }
                break;
            case ActionId::Resume: {
                const bool was = paused_ || override_;
                const bool hadOverride = override_;
                paused_ = false; override_ = false;
                if (was) {
                    dirty_ = true;
                    if (hadOverride) emit(EventKind::OverrideChanged, kAllOutputs, 0, -1, now, "off cause=resume");
                    emit(EventKind::Resumed, kAllOutputs, 0, cueIndex_, now, a.value == 1.0 ? "advance" : "continue");
                }
                if (a.value == 1.0) advanceNow(now);
                break;
            }
            case ActionId::SetMode: {
                const ShowMode m = static_cast<ShowMode>(static_cast<int>(a.value));
                if (m == mode_) break;
                HistEntry h; h.kind = HistEntry::Kind::Mode; h.label = "mode"; h.modeBefore = mode_; h.modeAfter = m; h.lastNs = now;
                setModeInternal(m, now, "set");
                pushHistory(std::move(h));
                break;
            }
            case ActionId::NextCue: {
                const int n = nextEligibleCue(cueIndex_, tun_.loopCues, true, now);
                if (n >= 0) enterCue(n, now);
                break;
            }
            case ActionId::PrevCue: {
                const int p = prevEligibleCue(cueIndex_);
                if (p >= 0) enterCue(p, now);
                break;
            }
            case ActionId::GoToCue: enterCue(static_cast<int>(a.value), now); break;
            // ---- blackout (latched; restore explicit)
            case ActionId::VisualBlackout: setBlackout(true, lightingBo_, emergencyBo_, now, "visual"); break;
            case ActionId::LightingBlackout: setBlackout(visualBo_, true, emergencyBo_, now, "lighting"); break;
            case ActionId::EmergencyBlackout: setBlackout(visualBo_, lightingBo_, true, now, "emergency"); break;
            case ActionId::RestoreBlackout: setBlackout(false, false, false, now, "restore"); break;
            // ---- look controls (undoable edits)
            case ActionId::Brightness: case ActionId::Speed: case ActionId::Intensity: case ActionId::ColorHue:
            case ActionId::ColorSaturation: case ActionId::EffectPrismMirror: case ActionId::EffectBloomPulse:
                applyParam(a, now);
                break;
            // ---- tempo / key
            case ActionId::SetBpm:
                tempoOvr_ = {true, a.value, ValueSource::Manual}; taps_.clear(); dirty_ = true;
                emit(EventKind::TempoOverrideChanged, kAllOutputs, 0, -1, now, "manual bpm=" + fmt2(a.value));
                break;
            case ActionId::TapTempo: tap(a.triggerTime != 0 ? a.triggerTime : receivedNs, now); break;
            case ActionId::ReleaseBpmOverride:
                if (tempoOvr_.active) { tempoOvr_ = TempoOvr{}; taps_.clear(); dirty_ = true; emit(EventKind::TempoOverrideChanged, kAllOutputs, 0, -1, now, "released"); }
                break;
            case ActionId::SetKey:
                keyOvr_ = {true, static_cast<int>(a.value) % 12, a.value >= 12}; dirty_ = true;
                emit(EventKind::KeyOverrideChanged, kAllOutputs, 0, -1, now, "manual tonic=" + std::to_string(keyOvr_.tonic) + (keyOvr_.minor ? " minor" : " major"));
                break;
            case ActionId::ReleaseKeyOverride:
                if (keyOvr_.active) { keyOvr_ = KeyOvr{}; dirty_ = true; emit(EventKind::KeyOverrideChanged, kAllOutputs, 0, -1, now, "released"); }
                break;
            // ---- strobe lock
            case ActionId::DisableStrobing: setStrobeLock(true, now); break;
            case ActionId::EnableStrobing: setStrobeLock(false, now); break;
            // ---- undo / redo / snapshots
            case ActionId::Undo: case ActionId::Redo: {
                const bool isUndo = id_ == ActionId::Undo;
                auto& from = isUndo ? undo_ : redo_;
                auto& to = isUndo ? redo_ : undo_;
                HistEntry h = std::move(from.back());
                from.pop_back();
                applyHist(h, isUndo, now);
                emit(isUndo ? EventKind::UndoApplied : EventKind::RedoApplied, kAllOutputs, 0, -1, now, h.label);
                to.push_back(std::move(h));
                break;
            }
            case ActionId::SnapshotStoreA: case ActionId::SnapshotStoreB: {
                targetRoots(a.target, tr);
                std::size_t ri = a.target == kAllOutputs ? 0 : tr[0];
                const int slot = id_ == ActionId::SnapshotStoreA ? 0 : 1;
                slots_[slot] = LookSlot{true, roots_[ri].sceneId, roots_[ri].look};
                dirty_ = true;
                emit(EventKind::SnapshotStored, roots_[ri].id, roots_[ri].sceneId, -1, now, slot == 0 ? "A" : "B");
                break;
            }
            case ActionId::SnapshotRecallA: case ActionId::SnapshotRecallB: {
                const int slot = id_ == ActionId::SnapshotRecallA ? 0 : 1;
                const LookSlot s = slots_[slot];
                targetRoots(a.target, tr);
                HistEntry h; h.kind = HistEntry::Kind::Look; h.label = slot == 0 ? "recall A" : "recall B"; h.lastNs = now;
                bool prim = false;
                for (std::size_t ri : tr) {
                    LookPart p; p.root = roots_[ri].id; p.hadBefore = roots_[ri].has; p.sceneBefore = roots_[ri].sceneId; p.pBefore = roots_[ri].look;
                    changeScene(ri, s.sceneId, tun_.manualTransition, tun_.manualTransitionSec, now, "recall");
                    roots_[ri].look = s.params;
                    p.sceneAfter = s.sceneId; p.pAfter = s.params;
                    h.looks.push_back(p);
                    prim = prim || ri == 0;
                }
                if (prim) engageOverride(now, "recall");
                emit(EventKind::SnapshotRecalled, kAllOutputs, s.sceneId, -1, now, slot == 0 ? "A" : "B");
                pushHistory(std::move(h));
                break;
            }
            case ActionId::SelectOutput:
                selectedOutput_ = a.value == -1.0 ? kAllOutputs : static_cast<OutputId>(a.value);
                dirty_ = true;
                break;
            default: break;
        }
    }

    void advanceNow(Ns now) {
        if (mode_ == ShowMode::Programmed) {
            const int n = nextEligibleCue(cueIndex_, tun_.loopCues, true, now);
            if (n >= 0) enterCue(n, now);
        } else {
            if (!autoPick(now, "resume-advance")) sceneElapsedNs_ = 0;
        }
    }

    void setBlackout(bool v, bool l, bool e, Ns now, const char* what) {
        if (v == visualBo_ && l == lightingBo_ && e == emergencyBo_) return;
        visualBo_ = v; lightingBo_ = l; emergencyBo_ = e; dirty_ = true;
        emit(EventKind::BlackoutChanged, kAllOutputs, 0, -1, now, what);
    }

    void setStrobeLock(bool locked, Ns now) {
        if (locked == strobeLocked_) return;
        strobeLocked_ = locked; dirty_ = true;
        if (locked) {
            const int safe = safeSceneIndex();
            for (std::size_t ri = 0; ri < roots_.size(); ++ri) {
                Root& r = roots_[ri];
                const model::SceneDef* sd = r.has ? scene(r.sceneId) : nullptr;
                if (sd && sd->strobe && safe >= 0)
                    changeScene(ri, doc_.scenes[static_cast<std::size_t>(safe)].id, model::TransitionKind::Cut, 0.0, now, "strobe-lock");
            }
            if (dropArmed_) { dropArmed_ = false; emit(EventKind::DropSkipped, roots_[0].id, 0, -1, now, "strobe lock"); }
        }
        emit(EventKind::StrobeLockChanged, kAllOutputs, 0, -1, now, locked ? "disabled" : "enabled");
    }

    void tap(Ns t, Ns now) {
        if (!taps_.empty()) {
            if (t <= taps_.back()) return;                                   // out-of-order / duplicate tap
            if (t - taps_.back() > tun_.tapResetNs) taps_.clear();
        }
        taps_.push_back(t);
        while (static_cast<int>(taps_.size()) > tun_.tapWindow) taps_.erase(taps_.begin());
        if (taps_.size() < 2) return;
        const double avg = static_cast<double>(taps_.back() - taps_.front()) / static_cast<double>(taps_.size() - 1);
        double bpm = 60e9 / avg;
        bpm = std::min(tun_.maxBpm, std::max(tun_.minBpm, bpm));
        tempoOvr_ = {true, bpm, ValueSource::Tap}; dirty_ = true;
        emit(EventKind::TempoOverrideChanged, kAllOutputs, 0, -1, now, "tap bpm=" + fmt2(bpm));
    }

    // ------------------------------------------------------------------ snapshot
    ShowSnapshot buildSnapshot(Ns now) {
        ShowSnapshot s;
        s.revision = revision_; s.atNs = now; s.mode = mode_; s.seed = doc_.seed;
        s.cueIndex = cueIndex_; s.cueCount = static_cast<std::int32_t>(doc_.cues.size());
        s.cueElapsedSec = nsToSec(cueElapsedNs_); s.cueElapsedBeats = cueElapsedBeats_;
        s.cueHolding = cueHolding_; s.cueWaitingForTempo = cueWaitingTempo_; s.programEnded = programEnded_;
        int nextCue = -1;
        if (mode_ == ShowMode::Programmed) nextCue = nextEligibleCueConst(cueIndex_, tun_.loopCues);
        for (std::size_t oi = 0; oi < doc_.outputs.size(); ++oi) {
            const Route& rt = routes_[oi];
            const Root& r = roots_[rt.rootIdx];
            OutputState o;
            o.id = doc_.outputs[oi].id;
            o.mirrorOf = rt.isRoot ? 0 : r.id;
            o.hasScene = r.has; o.sceneId = r.sceneId;
            double prog = 1.0;
            if (r.trDur > 0) prog = std::min(1.0, std::max(0.0, static_cast<double>(now - r.trStart) / static_cast<double>(r.trDur)));
            o.transitionProgress = prog;
            o.hasFrom = r.hasFrom && prog < 1.0; o.fromSceneId = r.fromId; o.transition = r.trKind;
            if (rt.rootIdx == 0 && nextCue >= 0) { o.hasNext = true; o.nextSceneId = doc_.cues[static_cast<std::size_t>(nextCue)].sceneId; }
            o.params = r.look;
            if (const model::SceneDef* sd = r.has ? scene(r.sceneId) : nullptr) {
                o.sceneSeed = sd->seed;
                if (strobeLocked_ && sd->strobe) o.strobeStripped = true;
            }
            if (strobeLocked_ && o.params.effectAmount[1] > tun_.strobeLockedBloomMax) {
                o.params.effectAmount[1] = tun_.strobeLockedBloomMax; o.strobeStripped = true;
            }
            s.outputs.push_back(o);
        }
        s.manualOverride = override_; s.paused = paused_;
        s.visualBlackout = visualBo_ || emergencyBo_; s.lightingBlackout = lightingBo_ || emergencyBo_;
        s.emergencyBlackout = emergencyBo_;
        s.strobingDisabled = strobeLocked_; s.lightingStrobeAllowed = !strobeLocked_;
        s.tempo = tempo_; s.key = key_; s.tempoOverride = tempoOvr_.active; s.keyOverride = keyOvr_.active;
        s.gridValid = tempoUsable();
        if (s.gridValid) { s.barIndex = static_cast<std::uint64_t>(std::max<std::int64_t>(0, grid_.barAt(now))); s.beatInBar = grid_.beatInBar(now); }
        s.bank = bank_; s.bankCount = bankCount(); s.selectedOutput = selectedOutput_;
        for (int i = 0; i < Limits::kSnapshotSlots; ++i) s.slots[i] = slots_[i];
        s.undoDepth = static_cast<std::uint32_t>(undo_.size()); s.redoDepth = static_cast<std::uint32_t>(redo_.size());
        s.wantedTier = wantedTier_; s.dropPending = dropArmed_;
        s.audioLost = audioMon_.lost();
        s.audioIdle = audioMon_.lost() && tun_.audioLossPolicy == AudioLossPolicy::Idle;
        return s;
    }
};

// ====================================================================== public API

ShowEngine::ShowEngine(const IClock* clock, Tunables t) : impl_(new Impl(clock, t)) {}
ShowEngine::~ShowEngine() = default;

Status ShowEngine::loadDocument(const model::ShowDocument& d) {
    Status v = validateDocument(d);
    if (!v.ok()) return v;
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->staged_ = d;
    impl_->stagedValid_ = true;
    return okStatus();
}

ActionResult ShowEngine::submit(const Action& a) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    Impl& m = *impl_;
    ++m.st_.submitted;
    ActionResult r;
    r.revision = m.revision_;
    const std::string why = m.validateAction(a, true);
    if (!why.empty()) { ++m.st_.rejected; r.reason = why; return r; }
    if (isSafetyCritical(static_cast<ActionId>(a.actionId))) {
        const Ns now = m.submitNow();
        m.applyAction(a, now, now, true);          // never queued, never dropped
        ++m.st_.criticalApplied; ++m.st_.accepted;
        r.accepted = true;
        return r;
    }
    if (m.inbox_.size() + m.pending_.size() >= Limits::kQueueCapacity) {
        ++m.st_.rejected; ++m.st_.queueRejected;
        r.reason = "action queue full";
        return r;
    }
    Queued q; q.a = a; q.receivedNs = m.submitNow();
    m.inbox_.push_back(std::move(q));
    ++m.st_.accepted;
    r.accepted = true;
    return r;
}

ActionResult ShowEngine::submitEdit(const EditCommand& e) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    Impl& m = *impl_;
    ++m.st_.submitted;
    ActionResult r;
    r.revision = m.revision_;
    const std::string why = m.validateEdit(e, true);
    if (!why.empty()) { ++m.st_.rejected; r.reason = why; return r; }
    if (m.inbox_.size() + m.pending_.size() >= Limits::kQueueCapacity) {
        ++m.st_.rejected; ++m.st_.queueRejected;
        r.reason = "action queue full";
        return r;
    }
    Queued q; q.isEdit = true; q.e = e; q.receivedNs = m.submitNow();
    m.inbox_.push_back(std::move(q));
    ++m.st_.accepted;
    r.accepted = true;
    return r;
}

void ShowEngine::setAudioAvailable(bool available) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->audioAvail_ = available;
}

void ShowEngine::setAudioLossPolicy(AudioLossPolicy p) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->tun_.audioLossPolicy = p;
}

void ShowEngine::markDownbeat(Ns at) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->grid_.setDownbeat(at);
}

ShowSnapshot ShowEngine::tick(Ns nowIn, const FeatureSnapshot& f) {
    ShowSnapshot snap;
    std::vector<model::JournalEntry> toSink;
    std::function<void(const model::JournalEntry&)> sink;
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        Impl& m = *impl_;
        const Ns now = m.started_ ? std::max(nowIn, m.lastTick_) : nowIn;
        const Ns dt = m.started_ ? now - m.lastTick_ : 0;
        if (!m.started_) { m.started_ = true; m.grid_.anchorIfUnset(now); }
        m.lastTick_ = now;
        m.feat_ = f;

        if (m.stagedValid_) m.installStaged(now);

        // wanted energy tier from the Energy Arc
        m.wantedTier_ = model::EnergyTier::Any;
        if (m.tun_.energyArcEnabled && !f.silent) {
            if (f.arcPhase == 2) m.wantedTier_ = model::EnergyTier::High;
            else if (f.energyArc < m.tun_.lowMax) m.wantedTier_ = model::EnergyTier::Low;
            else if (f.energyArc < m.tun_.mediumMax) m.wantedTier_ = model::EnergyTier::Medium;
            else m.wantedTier_ = model::EnergyTier::High;
        }

        // drain the inbox in submission order; non-immediate triggers wait in `pending_`
        std::vector<Queued> batch;
        batch.swap(m.inbox_);
        for (auto& q : batch) {
            if (!q.isEdit && q.a.trigger != TriggerKind::Immediate) {
                Pending p; p.q = q; p.submitNs = q.receivedNs;
                m.pending_.push_back(std::move(p));
            } else if (q.isEdit) {
                m.applyEdit(q.e, now);
            } else {
                m.applyAction(q.a, now, q.receivedNs, false);
            }
        }
        // effective tempo / key: operator override wins until released
        if (m.tempoOvr_.active) {
            m.tempo_ = TempoState{}; m.tempo_.bpm = m.tempoOvr_.bpm; m.tempo_.confidence = 1.f; m.tempo_.source = m.tempoOvr_.src;
        } else {
            m.tempo_ = f.tempo;
        }
        if (m.tempo_.valid()) m.grid_.setTempo(m.tempo_.bpm, now, m.tun_.regridBpmEpsilon);
        if (m.keyOvr_.active) {
            m.key_ = KeyState{}; m.key_.tonic = m.keyOvr_.tonic; m.key_.minor = m.keyOvr_.minor; m.key_.confidence = 1.f; m.key_.source = ValueSource::Manual;
        } else {
            m.key_ = f.key;
        }

        for (std::size_t i = 0; i < m.pending_.size();) {
            const Action& a = m.pending_[i].q.a;
            bool due = false;
            switch (a.trigger) {
                case TriggerKind::AtTime: due = now >= a.triggerTime; break;
                case TriggerKind::NextBeat: case TriggerKind::NextBar:
                    due = m.tempoUsable() ? now >= m.grid_.boundaryAfter(m.pending_[i].submitNs, a.trigger == TriggerKind::NextBar)
                                          : (now - m.pending_[i].submitNs) >= m.tun_.quantiseFallbackNs;
                    break;
                default: due = true; break;
            }
            if (due) {
                Queued q = std::move(m.pending_[i].q);
                m.pending_.erase(m.pending_.begin() + static_cast<std::ptrdiff_t>(i));
                m.applyAction(q.a, now, q.receivedNs, false);
            } else {
                ++i;
            }
        }

        // audio health (REQ-D09): debounced loss; transient state, never a document edit
        {
            const AudioHealthState ah = m.audioMon_.update(now, m.audioAvail_);
            if (ah.changed)
                m.emit(ah.lost ? EventKind::AudioLost : EventKind::AudioRestored, kAllOutputs, 0, -1, now,
                       m.tun_.audioLossPolicy == AudioLossPolicy::Idle ? "policy=idle" : "policy=continue");
        }

        // automation
        m.dropStep(now);
        if (!m.paused_ && !m.override_) {
            if (m.mode_ == ShowMode::Programmed) m.programmedStep(now, dt);
            else m.autoStep(now, dt);
        }

        if (m.dirty_) { ++m.revision_; m.dirty_ = false; }
        snap = m.buildSnapshot(now);
        snap.events = std::move(m.events_);
        m.events_.clear();
        snap.eventsDropped = m.eventsDropped_;
        m.eventsDropped_ = 0;
        m.last_ = snap;
        m.st_.queued = m.inbox_.size(); m.st_.pending = m.pending_.size();

        sink = m.sink_;
        if (sink) {
            toSink.swap(m.jbuf_);
        } else {
            for (auto& j : m.jbuf_) {
                if (m.jq_.size() < Limits::kJournalCapacity) m.jq_.push_back(std::move(j));
                else ++m.st_.journalDropped;
            }
            m.jbuf_.clear();
        }
    }
    if (sink) for (const auto& j : toSink) sink(j);
    return snap;
}

ShowSnapshot ShowEngine::latest() const {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    return impl_->last_;
}

model::ShowDocument ShowEngine::document() const {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    model::ShowDocument d = impl_->doc_;
    d.mode = impl_->mode_;
    d.strobingDisabled = impl_->strobeLocked_;
    return d;
}

void ShowEngine::setJournalSink(std::function<void(const model::JournalEntry&)> sink) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->sink_ = std::move(sink);
}

std::size_t ShowEngine::drainJournal(std::vector<model::JournalEntry>& out, std::size_t max) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    std::size_t n = 0;
    while (n < max && !impl_->jq_.empty()) { out.push_back(std::move(impl_->jq_.front())); impl_->jq_.pop_front(); ++n; }
    return n;
}

Stats ShowEngine::stats() const {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    Stats s = impl_->st_;
    s.queued = impl_->inbox_.size(); s.pending = impl_->pending_.size();
    return s;
}

const Tunables& ShowEngine::tunables() const { return impl_->tun_; }

}  // namespace ss::show
