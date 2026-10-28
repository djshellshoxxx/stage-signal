// StageSignal show engine: complete document validation (IF-11: validate before state swap).
#include <cmath>
#include <set>
#include <unordered_map>
#include "ss/show/engine.hpp"

namespace ss::show {

const char* eventKindName(EventKind k) {
    switch (k) {
        case EventKind::DocumentLoaded: return "DocumentLoaded";
        case EventKind::SceneChanged: return "SceneChanged";
        case EventKind::CueFired: return "CueFired";
        case EventKind::CueSkipped: return "CueSkipped";
        case EventKind::CueListEnded: return "CueListEnded";
        case EventKind::DropScheduled: return "DropScheduled";
        case EventKind::Drop: return "Drop";
        case EventKind::DropSkipped: return "DropSkipped";
        case EventKind::ModeChanged: return "ModeChanged";
        case EventKind::OverrideChanged: return "OverrideChanged";
        case EventKind::Paused: return "Paused";
        case EventKind::Resumed: return "Resumed";
        case EventKind::BlackoutChanged: return "BlackoutChanged";
        case EventKind::StrobeLockChanged: return "StrobeLockChanged";
        case EventKind::TempoOverrideChanged: return "TempoOverrideChanged";
        case EventKind::KeyOverrideChanged: return "KeyOverrideChanged";
        case EventKind::SnapshotStored: return "SnapshotStored";
        case EventKind::SnapshotRecalled: return "SnapshotRecalled";
        case EventKind::UndoApplied: return "UndoApplied";
        case EventKind::RedoApplied: return "RedoApplied";
        case EventKind::AudioLost: return "AudioLost";
        case EventKind::AudioRestored: return "AudioRestored";
        case EventKind::EditApplied: return "EditApplied";
        case EventKind::ActionRejected: return "ActionRejected";
    }
    return "Unknown";
}

namespace {
bool unit(float v) { return std::isfinite(v) && v >= 0.f && v <= 1.f; }
Status bad(const std::string& why) { return makeError(ErrorCategory::InvalidArgument, why, Recoverability::OperatorAction); }
}  // namespace

Status ShowEngine::validateDocument(const model::ShowDocument& d) {
    using model::Limits;
    if (d.schemaMajor != model::kSchemaMajor) return makeError(ErrorCategory::SchemaUnsupported, "unsupported schema major version");
    if (d.scenes.size() > Limits::kMaxScenes) return bad("too many scenes");
    if (d.cues.size() > Limits::kMaxCues) return bad("too many cues");
    if (d.outputs.empty()) return bad("document has no outputs");
    if (d.outputs.size() > ss::show::Limits::kMaxOutputs) return bad("too many outputs");
    if (static_cast<int>(d.mode) > 2) return bad("invalid mode");
    if (d.minDwellSec < 0 || d.minDwellSec > 3600) return bad("minDwellSec out of range");
    if (d.repeatHistory < 0 || d.repeatHistory > static_cast<int>(ss::show::Limits::kRepeatHistoryMax)) return bad("repeatHistory out of range");

    std::set<std::uint32_t> sceneIds;
    for (const auto& s : d.scenes) {
        if (!sceneIds.insert(s.id).second) return bad("duplicate scene id");
        if (static_cast<int>(s.kind) >= static_cast<int>(model::SceneKind::Count)) return bad("invalid scene kind");
        if (static_cast<int>(s.tier) > static_cast<int>(model::EnergyTier::Drop)) return bad("invalid energy tier");
        const auto& p = s.params;
        if (!unit(p.brightness) || !unit(p.speed) || !unit(p.intensity) || !unit(p.hue) || !unit(p.saturation) ||
            !unit(p.effectAmount[0]) || !unit(p.effectAmount[1]))
            return bad("scene parameter out of range");
        if (p.prismSegments < 2 || p.prismSegments > 16) return bad("prismSegments out of range");
    }
    std::set<std::uint32_t> cueIds;
    for (const auto& c : d.cues) {
        if (!cueIds.insert(c.id).second) return bad("duplicate cue id");
        if (!sceneIds.count(c.sceneId)) return bad("cue references a missing scene");
        if (!std::isfinite(c.duration) || c.duration < 0 || c.duration > 86400.0 * 7) return bad("cue duration out of range");
        if (!std::isfinite(c.transitionDuration) || c.transitionDuration < 0 || c.transitionDuration > 60.0) return bad("transition duration out of range");
        if (static_cast<int>(c.transition) > 2) return bad("invalid transition");
    }
    std::unordered_map<OutputId, const model::OutputDef*> outs;
    for (const auto& o : d.outputs) {
        if (o.id == kAllOutputs || o.id == 0) return bad("invalid output id");
        if (!outs.emplace(o.id, &o).second) return bad("duplicate output id");
    }
    for (const auto& o : d.outputs) {
        if (o.kind != RouteKind::Duplicate) continue;
        // duplicateOf must resolve (through other duplicates) to a non-duplicate output; no cycles.
        OutputId cur = o.duplicateOf;
        for (std::size_t hop = 0; hop <= d.outputs.size(); ++hop) {
            auto it = outs.find(cur);
            if (it == outs.end() || cur == o.id) return bad("duplicate output has no valid source");
            if (it->second->kind != RouteKind::Duplicate) { cur = 0; break; }
            cur = it->second->duplicateOf;
        }
        if (cur != 0) return bad("duplicate output cycle");
    }
    bool anyRoot = false;
    for (const auto& o : d.outputs) anyRoot = anyRoot || o.kind != RouteKind::Duplicate;
    if (!anyRoot) return bad("document needs at least one non-duplicate output");
    return okStatus();
}

}  // namespace ss::show
