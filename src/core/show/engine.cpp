#include "core/show/engine.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cmath>

namespace ss {
namespace show {

namespace {
using json = nlohmann::json;
ActionResult R(bool ok, const std::string& why = std::string()) { ActionResult r; r.accepted = ok; r.reason = why; return r; }
bool isInt(double v) { return std::isfinite(v) && v == std::floor(v); }
bool operatorSource(ActionSource s) { return s == ActionSource::UI || s == ActionSource::Keyboard || s == ActionSource::Midi || s == ActionSource::Remote; }
bool immediateOnly(ActionId id) {
  switch (id) {
    case ActionId::VisualBlackout: case ActionId::LightingBlackout: case ActionId::EmergencyBlackout: case ActionId::RestoreBlackout:
    case ActionId::PauseAutomation: case ActionId::DisableStrobing: case ActionId::TapTempo: case ActionId::SetTarget:
      return true;
    default: return false;
  }
}
bool revisionExempt(ActionId id) {   // safety actions must never be refused for a stale UI revision
  switch (id) {
    case ActionId::VisualBlackout: case ActionId::LightingBlackout: case ActionId::EmergencyBlackout: case ActionId::RestoreBlackout:
    case ActionId::PauseAutomation: case ActionId::DisableStrobing: case ActionId::DisarmLighting:
      return true;
    default: return false;
  }
}
std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
bool strobeLike(const json& j, int depth) {
  if (depth > 16) return false;
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) {
      std::string k = lower(it.key());
      const json& v = it.value();
      if ((k == "strobe" || k == "flash") && ((v.is_boolean() && v.get<bool>()) || (v.is_number() && v.get<double>() > 0))) return true;
      if ((k == "type" || k == "preset" || k == "function" || k == "kind") && v.is_string()) {
        std::string s = lower(v.get<std::string>());
        if (s.find("strobe") != std::string::npos || s.find("flash") != std::string::npos) return true;
      }
      if (strobeLike(v, depth + 1)) return true;
    }
  } else if (j.is_array()) {
    for (const auto& e : j) if (strobeLike(e, depth + 1)) return true;
  }
  return false;
}
} // namespace

bool ShowEngine::isShowAction(ActionId id) {
  switch (id) {
    case ActionId::None: case ActionId::StartRecording: case ActionId::StopRecording: case ActionId::AddMarker:
    case ActionId::StartBroadcast: case ActionId::StopBroadcast: case ActionId::Count_:
      return false;
    default: return true;
  }
}

ShowEngine::ShowEngine(Options o) : opt_(o) { resetRuntime(); }

void ShowEngine::emit(const char* kind, const std::string& js) { if (hook_) hook_(kind, js); }

void ShowEngine::commit() { if (dirty_) { ++rev_; dirty_ = false; } }

uint64_t ShowEngine::rngNext() {   // splitmix64
  rng_ += 0x9E3779B97F4A7C15ull;
  uint64_t z = rng_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
void ShowEngine::rngSeed(uint64_t s) { rng_ = s; }

int ShowEngine::sceneIndexById(const std::string& id) const {
  for (size_t i = 0; i < doc_.scenes.size(); ++i) if (doc_.scenes[i].id == id) return (int)i;
  return -1;
}
bool ShowEngine::sceneAllowed(int idx) const {
  if (idx < 0 || idx >= (int)doc_.scenes.size()) return false;
  return !(strobeOff_ && doc_.scenes[(size_t)idx].strobe);
}

void ShowEngine::resetRuntime() {
  mode_ = doc_.defaultMode == 1 ? ShowMode::Programmed : doc_.defaultMode == 2 ? ShowMode::Hybrid : ShowMode::Auto;
  paused_ = override_ = false; modeRestore_ = -1;
  outgoing_ = -1; transProg_ = 1.0f; transNs_ = 0; transType_.clear();
  scene_ = -1;
  for (int i = 0; i < (int)doc_.scenes.size(); ++i) if (sceneAllowed(i) || !(strobeOff_ || doc_.disableStrobing)) { scene_ = i; break; }
  outs_.clear();
  for (const auto& o : doc_.outputs) { OutputState s; s.logicalId = o.logicalId; s.role = o.role; outs_.push_back(s); }
  if (outs_.empty()) { OutputState s; s.logicalId = "main"; outs_.push_back(s); }
  outSceneOverride_.assign(outs_.size(), -1);
  target_ = -1; bank_ = 0;
  strobeOff_ = strobeOff_ || doc_.disableStrobing;
  if (strobeOff_ && scene_ >= 0 && !sceneAllowed(scene_)) { scene_ = -1; for (int i = 0; i < (int)doc_.scenes.size(); ++i) if (sceneAllowed(i)) { scene_ = i; break; } }
  armReq_ = false;                       // loading a show never arms lighting
  cue_ = -1; cueEnded_ = false; cueElapsed_ = 0; cueBeats_ = 0; cueWaitTempo_ = false; cueMediaMissing_ = false; pendingCueIdx_ = -1;
  seed_ = doc_.autoCfg.seed; rngSeed(seed_);
  history_.clear(); if (scene_ >= 0) history_.push_back(scene_);
  startDwell();
  bpmManual_ = doc_.manualBpm > 0; bpmVal_ = doc_.manualBpm; bpmSrc_ = Estimate::Manual; bpmSetNs_ = evt_;
  taps_.clear(); anchorValid_ = bpmManual_; anchor_ = evt_; anchorBpm_ = bpmVal_;
  keyManual_ = doc_.manualKey >= 0; keyPc_ = keyManual_ ? doc_.manualKey % 12 : 0; keyMinor_ = keyManual_ && doc_.manualKey >= 12; keySetNs_ = evt_;
  draft_.clear(); preview_.clear(); pending_.clear();
  idle_ = opt_.startIdle; idleFromAudio_ = false;
  mergeFeatures();
}

Status ShowEngine::replaceDocument(const ShowDocument& d) {
  Status s = validateShow(d);
  if (!s.ok()) { s.revision = rev_; return s; }     // live state untouched
  doc_ = d;
  resetRuntime();
  dirty_ = true; commit();
  emit("show_loaded", json{{"uuid", doc_.uuid}, {"scenes", doc_.scenes.size()}, {"cues", doc_.cues.size()}}.dump());
  Status ok = Status::Ok(); ok.revision = rev_; return ok;
}
void ShowEngine::adoptUnvalidated(const ShowDocument& d) { doc_ = d; resetRuntime(); dirty_ = true; commit(); }

ShowDocument ShowEngine::documentWithState() const {
  ShowDocument d = doc_;
  d.manualBpm = bpmManual_ ? bpmVal_ : 0;
  d.manualKey = keyManual_ ? keyPc_ + (keyMinor_ ? 12 : 0) : -1;
  d.disableStrobing = strobeOff_;
  return d;
}

void ShowEngine::setSeed(uint64_t s) { seed_ = s; rngSeed(s); history_.clear(); if (scene_ >= 0) history_.push_back(scene_); startDwell(); dirty_ = true; commit(); }

void ShowEngine::startDwell() {
  const AutoConfig& a = doc_.autoCfg;
  Ns extra = 0;
  if (a.maxDwellMs > a.minDwellMs) extra = (Ns)(rngNext() % (uint64_t)(a.maxDwellMs - a.minDwellMs + 1));
  dwellTarget_ = ((Ns)a.minDwellMs + extra) * kNsPerMs;
  dwell_ = 0; autoWaitUntil_ = -1;
}

void ShowEngine::setScene(int idx, const Transition& tr, const char* reason) {
  if (idx == scene_ || idx < 0) { bool had = false; for (int& o : outSceneOverride_) if (o != -1) { o = -1; had = true; } if (had) dirty_ = true; return; }
  outgoing_ = (tr.ms > 0 && scene_ >= 0) ? scene_ : -1;
  transStart_ = now_; transNs_ = (Ns)tr.ms * kNsPerMs; transType_ = tr.type; transProg_ = tr.ms > 0 ? 0.0f : 1.0f;
  int from = scene_;
  scene_ = idx;
  for (int& o : outSceneOverride_) o = -1;
  history_.push_back(idx); if (history_.size() > 64) history_.erase(history_.begin());
  startDwell();
  dirty_ = true;
  emit("scene_change", json{{"scene", doc_.scenes[(size_t)idx].id}, {"from", from >= 0 ? doc_.scenes[(size_t)from].id : std::string()}, {"reason", reason}, {"cue", cue_}}.dump());
}

int ShowEngine::stepScene(int dir) const {
  int n = (int)doc_.scenes.size();
  if (n == 0) return -1;
  int from = scene_;
  for (int k = 1; k <= n; ++k) {
    int idx = from < 0 ? (dir > 0 ? k - 1 : n - k) : (((from + dir * k) % n) + n) % n;
    if (sceneAllowed(idx)) return idx;
  }
  return -1;
}

int ShowEngine::pickNext() {
  struct C { int idx; int w; };
  std::vector<C> valid;
  if (doc_.autoCfg.pool.empty()) { for (int i = 0; i < (int)doc_.scenes.size(); ++i) if (sceneAllowed(i)) valid.push_back({i, 1}); }
  else for (const auto& p : doc_.autoCfg.pool) { int i = sceneIndexById(p.sceneId); if (i >= 0 && p.weight > 0 && sceneAllowed(i)) valid.push_back({i, p.weight}); }
  if (valid.empty()) return -1;
  int n = (int)valid.size();
  int window = std::max(1, std::min(doc_.autoCfg.repeatHistory, n - 1));
  std::vector<C> cands;
  for (const C& c : valid) {
    bool recent = false;
    for (int k = 0; k < window && k < (int)history_.size(); ++k) if (history_[history_.size() - 1 - (size_t)k] == c.idx) { recent = true; break; }
    if (!recent && c.idx != scene_) cands.push_back(c);
  }
  if (cands.empty()) for (const C& c : valid) if (c.idx != scene_) cands.push_back(c);   // relax history, never repeat on-air scene
  if (cands.empty()) return -1;
  if (cands.size() == 1) return cands[0].idx;
  uint64_t total = 0; for (const C& c : cands) total += (uint64_t)c.w;
  uint64_t r = rngNext() % total;
  for (const C& c : cands) { if (r < (uint64_t)c.w) return c.idx; r -= (uint64_t)c.w; }
  return cands.back().idx;
}

Ns ShowEngine::nextBoundary(Ns from, bool bar) const {
  if (!anchorValid_ || anchorBpm_ <= 0) return from;
  Ns period = (Ns)(60e9 / anchorBpm_);
  if (period <= 0) return from;
  if (bar) period *= doc_.autoCfg.beatsPerBar;
  Ns d = from - anchor_;
  Ns k = d >= 0 ? (d + period - 1) / period : -((-d) / period);
  return anchor_ + k * period;
}

// ---- cues ------------------------------------------------------------------------------------
bool ShowEngine::beginCue(int idx, const char* reason) {
  if (idx < 0 || idx >= (int)doc_.cues.size()) return false;
  const Cue& c = doc_.cues[(size_t)idx];
  int sidx = -2;
  if (!c.sceneId.empty()) {
    sidx = sceneIndexById(c.sceneId);
    if (sidx < 0) { emit("cue_skipped", json{{"cue", c.id}, {"index", idx}, {"why", "scene missing"}}.dump()); return false; }
    if (!sceneAllowed(sidx)) { emit("cue_skipped", json{{"cue", c.id}, {"index", idx}, {"why", "strobe disabled"}}.dump()); return false; }
  }
  cue_ = idx; cueEnded_ = false; cueElapsed_ = 0; cueBeats_ = 0; cueWaitTempo_ = false; pendingCueIdx_ = -1;
  cueMediaMissing_ = false;
  if (!c.mediaRef.empty()) {
    cueMediaMissing_ = true;
    for (const auto& m : doc_.media) if (m.hash == c.mediaRef) { cueMediaMissing_ = false; break; }
    if (cueMediaMissing_) emit("media_missing", json{{"cue", c.id}}.dump());
  }
  if (sidx >= 0) setScene(sidx, c.transition, reason);
  dirty_ = true;
  emit("cue_start", json{{"cue", c.id}, {"index", idx}, {"reason", reason}}.dump());
  return true;
}

bool ShowEngine::beginFirstAvailable(int from, int dir, const char* reason) {
  int n = (int)doc_.cues.size();
  if (n == 0) return false;
  int idx = from;
  for (int k = 0; k < n; ++k) {
    if (idx >= n) { if (doc_.cueEnd == CueListEnd::Loop) idx = 0; else return false; }
    if (idx < 0) return false;
    if (beginCue(idx, reason)) return true;
    idx += dir;
  }
  return false;
}

void ShowEngine::endCueRelease() {
  if (cue_ < 0) return;
  cue_ = -1; cueEnded_ = false; cueMediaMissing_ = false; cueWaitTempo_ = false; pendingCueIdx_ = -1;
  startDwell(); dirty_ = true;
  emit("cue_release", "{}");
}

void ShowEngine::tickCues(Ns dt) {
  if (mode_ == ShowMode::Auto) return;
  int n = (int)doc_.cues.size();
  if (pendingCueIdx_ >= 0) {
    if (now_ >= pendingCueAt_) {
      int idx = pendingCueIdx_; pendingCueIdx_ = -1;
      if (!beginFirstAvailable(idx, 1, "schedule")) cueEnded_ = true;
    }
    return;
  }
  if (cue_ < 0) {
    if (mode_ == ShowMode::Programmed && !cueEnded_ && n > 0) { if (!beginFirstAvailable(0, 1, "schedule")) cueEnded_ = true; }
    return;
  }
  if (cueEnded_) return;
  cueElapsed_ += dt;
  for (int guard = 0; guard <= n + 1; ++guard) {
    const Cue& c = doc_.cues[(size_t)cue_];
    bool done = false; Ns overflow = 0; double overflowBeats = 0;
    if (c.duration.beats) {
      if (tempoValid()) { if (guard == 0) cueBeats_ += nsToSec(dt) * effTempo_.value / 60.0; cueWaitTempo_ = false; }
      else cueWaitTempo_ = true;
      if (c.duration.beatCount > 0 && cueBeats_ >= c.duration.beatCount) { done = true; overflowBeats = cueBeats_ - c.duration.beatCount; }
    } else if (c.duration.ms > 0) {
      Ns dur = c.duration.ms * kNsPerMs;
      if (cueElapsed_ >= dur) { done = true; overflow = cueElapsed_ - dur; }
    }
    if (!done) return;
    if (mode_ == ShowMode::Hybrid) { endCueRelease(); return; }
    int next = cue_ + 1;
    if (next >= n) {
      if (doc_.cueEnd == CueListEnd::Loop) next = 0;
      else { cueEnded_ = true; dirty_ = true; emit("cue_list_end", "{}"); return; }
    }
    const Cue& nc = doc_.cues[(size_t)next];
    if (nc.quantize != Quantize::Immediate && tempoValid() && anchorValid_) {
      pendingCueIdx_ = next; pendingCueAt_ = nextBoundary(now_, nc.quantize == Quantize::Bar); dirty_ = true;
      return;
    }
    if (!beginFirstAvailable(next, 1, "schedule")) { cueEnded_ = true; dirty_ = true; return; }
    cueElapsed_ = overflow; cueBeats_ = overflowBeats;
  }
}

void ShowEngine::tickAuto(Ns dt) {
  if (!(mode_ == ShowMode::Auto || (mode_ == ShowMode::Hybrid && cue_ < 0))) return;
  dwell_ += dt;
  if (dwell_ < dwellTarget_) return;
  Quantize q = doc_.autoCfg.quantize;
  if (q != Quantize::Immediate && tempoValid() && anchorValid_) {
    if (autoWaitUntil_ < 0) autoWaitUntil_ = nextBoundary(now_, q == Quantize::Bar);
    if (now_ < autoWaitUntil_) return;
  }
  int pick = pickNext();
  if (pick >= 0) setScene(pick, doc_.autoCfg.transition, "auto");
  else startDwell();   // pool of one (or nothing eligible): stay, rearm
}

// ---- tempo/key/audio ---------------------------------------------------------------------------
void ShowEngine::mergeFeatures() {
  if (bpmManual_) {
    effTempo_ = Estimate(); effTempo_.valid = true; effTempo_.confidence = 1; effTempo_.value = bpmVal_;
    effTempo_.ageNs = std::max<Ns>(0, evt_ - bpmSetNs_); effTempo_.source = bpmSrc_;
    if (!anchorValid_) { anchorValid_ = true; anchor_ = evt_; anchorBpm_ = bpmVal_; }
  } else {
    effTempo_ = feat_.tempo; effTempo_.source = Estimate::Estimated;
    if (effTempo_.valid && effTempo_.value > 0) {
      if (!anchorValid_ || std::fabs(effTempo_.value - anchorBpm_) > 0.5) { anchorValid_ = true; anchor_ = now_; anchorBpm_ = effTempo_.value; }
    }
  }
  if (keyManual_) {
    effKey_ = Estimate(); effKey_.valid = true; effKey_.confidence = 1; effKey_.value = keyPc_; effKey_.minor = keyMinor_;
    effKey_.ageNs = std::max<Ns>(0, evt_ - keySetNs_); effKey_.source = Estimate::Manual;
  } else { effKey_ = feat_.key; effKey_.source = Estimate::Estimated; }
}

FeatureSnapshot ShowEngine::effectiveFeatures() const {
  FeatureSnapshot f = feat_;
  f.tempo = effTempo_; f.key = effKey_;
  return f;
}

void ShowEngine::setAudioAvailable(bool avail) {
  explicitLost_ = !avail;
  if (!avail && !audioLost_) {
    audioLost_ = true; dirty_ = true;
    if (doc_.idle.audioLoss == AudioLossPolicy::Idle) { idle_ = true; idleFromAudio_ = true; }
    emit("audio_loss", json{{"policy", doc_.idle.audioLoss == AudioLossPolicy::Idle ? "idle" : "continue"}}.dump());
    commit();
  }
}

void ShowEngine::update(Ns now, const FeatureSnapshot& f) {
  Ns dt = haveNow_ ? now - now_ : 0;
  if (dt < 0) dt = 0;
  if (!haveNow_ || now > now_) now_ = now;
  haveNow_ = true; evt_ = now_;
  feat_ = f;
  if (!haveSeq_ || f.sequence != lastSeq_) { haveSeq_ = true; lastSeq_ = f.sequence; lastSeqNs_ = now_; }
  bool stale = (now_ - lastSeqNs_) >= (Ns)doc_.idle.audioLossMs * kNsPerMs;
  bool lost = stale || explicitLost_;
  if (lost != audioLost_) {
    audioLost_ = lost; dirty_ = true;
    if (lost) {
      if (doc_.idle.audioLoss == AudioLossPolicy::Idle) { idle_ = true; idleFromAudio_ = true; }
      emit("audio_loss", json{{"policy", doc_.idle.audioLoss == AudioLossPolicy::Idle ? "idle" : "continue"}}.dump());
    } else {
      if (idleFromAudio_) { idle_ = false; idleFromAudio_ = false; }
      emit("audio_restored", "{}");
    }
  }
  bool hadTempo = effTempo_.valid, hadKey = effKey_.valid; Estimate::Source ts = effTempo_.source;
  mergeFeatures();
  if (hadTempo != effTempo_.valid || hadKey != effKey_.valid || ts != effTempo_.source) dirty_ = true;
  runPending();
  if (transProg_ < 1.0f || outgoing_ >= 0) {
    if (transNs_ <= 0) { transProg_ = 1.0f; outgoing_ = -1; dirty_ = true; }
    else {
      double p = (double)(now_ - transStart_) / (double)transNs_;
      if (p >= 1.0) { transProg_ = 1.0f; outgoing_ = -1; dirty_ = true; }
      else transProg_ = (float)(p < 0 ? 0 : p);
    }
  }
  if (automationRunning()) { tickCues(dt); tickAuto(dt); }
  if (lastTkJournal_ < 0 || now_ - lastTkJournal_ >= kNsPerSec) {
    lastTkJournal_ = now_;
    json j; j["bpm"] = effTempo_.valid ? json(effTempo_.value) : json(nullptr);
    j["tempoSrc"] = effTempo_.source == Estimate::Estimated ? "estimated" : effTempo_.source == Estimate::Tap ? "tap" : "manual";
    j["key"] = effKey_.valid ? json(effKey_.value + (effKey_.minor ? 12 : 0)) : json(nullptr);
    emit("tempo_key", j.dump());
  }
  commit();
}

ActionResult ShowEngine::tap(Ns t) {
  if (!taps_.empty() && t <= taps_.back()) return R(false, "tap timestamp not increasing");
  if (!taps_.empty() && (t - taps_.back()) > (Ns)opt_.tapResetGapMs * kNsPerMs) taps_.clear();
  if (!taps_.empty() && (t - taps_.back()) < 200 * kNsPerMs) return R(false, "tap too fast (debounce)");   // > 300 BPM
  taps_.push_back(t);
  if ((int)taps_.size() > opt_.tapWindow) taps_.erase(taps_.begin());
  if (taps_.size() < 2) return R(true, "tap 1 recorded; need one more");
  double span = (double)(taps_.back() - taps_.front()) / 1e9;
  double bpm = 60.0 * (double)(taps_.size() - 1) / span;
  if (bpm < 30.0 || bpm > 300.0) { taps_.clear(); taps_.push_back(t); return R(false, "tap tempo out of range 30..300"); }
  bool changed = !bpmManual_ || bpmSrc_ != Estimate::Tap || std::fabs(bpmVal_ - bpm) > 1e-9;
  bpmManual_ = true; bpmSrc_ = Estimate::Tap; bpmVal_ = bpm; bpmSetNs_ = t;
  anchorValid_ = true; anchor_ = t; anchorBpm_ = bpm;
  if (changed) { dirty_ = true; emit("tempo_override", json{{"bpm", bpm}, {"source", "tap"}, {"taps", taps_.size()}}.dump()); }
  return R(true);
}

// ---- outputs/text ---------------------------------------------------------------------------------
void ShowEngine::resolveTargets(int t, std::vector<int>& out) const {
  out.clear();
  if (t == -1) { for (int i = 0; i < (int)outs_.size(); ++i) if (outs_[(size_t)i].role != OutputRole::Controls) out.push_back(i); }
  else if (t >= 0 && t < (int)outs_.size() && outs_[(size_t)t].role != OutputRole::Controls) out.push_back(t);
}

void ShowEngine::publishTo(int target, const std::string& id, const std::string& text) {
  std::vector<int> ts; resolveTargets(target, ts);
  bool any = false;
  for (int i : ts) { OutputState& o = outs_[(size_t)i]; if (o.publishedText != text || o.publishedTextId != id) { o.publishedText = text; o.publishedTextId = id; any = true; } }
  if (any) { dirty_ = true; emit(id.empty() ? "text_clear" : "text_publish", json{{"id", id}, {"outputs", ts.size()}}.dump()); }
}

Status ShowEngine::setDraft(const std::string& t) {
  size_t cps = 0;
  if (!utf8Valid(t, &cps)) return Status::Fail(Err::InvalidArgument, "draft is not valid UTF-8");
  if (cps > limits::kMaxTextCodePoints) return Status::Fail(Err::InvalidArgument, "draft exceeds 4096 code points");
  if (t.find('\0') != std::string::npos) return Status::Fail(Err::InvalidArgument, "NUL not allowed");
  if (t != draft_) { draft_ = t; dirty_ = true; }     // audience outputs untouched
  commit();
  return Status::Ok();
}
Status ShowEngine::previewDraft() { if (preview_ != draft_) { preview_ = draft_; dirty_ = true; } commit(); return Status::Ok(); }
Status ShowEngine::publishDraft(int target) {
  if (preview_ != draft_) return Status::Fail(Err::InvalidArgument, "draft changed since preview; preview before publishing");
  if (draft_.empty()) return Status::Fail(Err::InvalidArgument, "nothing to publish");
  std::vector<int> ts; resolveTargets(target == -1 ? target_ : target, ts);
  if (ts.empty()) return Status::Fail(Err::InvalidArgument, "no visual output for target");
  publishTo(target == -1 ? target_ : target, "draft", draft_);
  commit();
  return Status::Ok();
}

bool ShowEngine::mappingAllowed(const std::string& m) const {
  if (!strobeOff_) return true;
  json j = json::parse(m, nullptr, false);
  if (j.is_discarded()) return false;
  return !strobeLike(j, 0);
}

// ---- actions ---------------------------------------------------------------------------------------
void ShowEngine::operatorOverride(const char* why) {
  if (override_) return;
  override_ = true; dirty_ = true;
  if (mode_ == ShowMode::Auto) { mode_ = ShowMode::Hybrid; modeRestore_ = (int)ShowMode::Auto; }
  emit("override", json{{"why", why}}.dump());
}

void ShowEngine::enterMode(ShowMode m) {
  modeRestore_ = -1;
  if (mode_ == m) return;
  mode_ = m; dirty_ = true;
  pendingCueIdx_ = -1;
  if (m == ShowMode::Auto) { cue_ = -1; cueEnded_ = false; cueMediaMissing_ = false; startDwell(); }
  else if (m == ShowMode::Programmed) { cueEnded_ = false; }
  emit("mode_change", json{{"mode", m == ShowMode::Auto ? "auto" : m == ShowMode::Programmed ? "programmed" : "hybrid"}}.dump());
}

void ShowEngine::resume(bool next) {
  bool changed = paused_ || override_;
  paused_ = false; override_ = false;
  if (modeRestore_ >= 0) { mode_ = (ShowMode)modeRestore_; modeRestore_ = -1; changed = true; }
  clearManualIdle();
  if (next) {
    if (mode_ != ShowMode::Auto && !doc_.cues.empty()) {
      int idx = cue_ < 0 ? 0 : cue_ + 1;
      if (!beginFirstAvailable(idx, 1, "resume_next") && mode_ == ShowMode::Programmed) cueEnded_ = true;
    } else {
      int p = pickNext();
      if (p >= 0) setScene(p, doc_.autoCfg.transition, "resume_next");
    }
  }
  if (changed || next) { dirty_ = true; emit("resume", json{{"next", next}}.dump()); }
}

ActionResult ShowEngine::submit(const Action& a) { return submit(a, haveNow_ ? now_ : 0); }

ActionResult ShowEngine::submit(const Action& a, Ns now) {
  evt_ = now;
  uint64_t before = rev_;
  ActionResult r = submitInner(a);
  bool tempoWas = effTempo_.valid;
  mergeFeatures();
  if (tempoWas != effTempo_.valid) dirty_ = true;
  commit();
  r.revision = rev_;
  if (r.accepted && r.reason.empty() && rev_ == before) r.reason = "no change";
  if (!r.accepted) emit("action_rejected", json{{"id", (int)a.id}, {"reason", r.reason}}.dump());
  return r;
}

void ShowEngine::runPending() {
  if (pending_.empty()) return;
  std::vector<Pending> keep, due;
  for (auto& p : pending_) (now_ >= p.at ? due : keep).push_back(p);
  pending_ = keep;
  for (auto& p : due) {
    ActionResult r = apply(p.a, true);
    if (!r.accepted) emit("pending_rejected", json{{"id", (int)p.a.id}, {"reason", r.reason}}.dump());
  }
  if (!due.empty()) dirty_ = true;
}

ActionResult ShowEngine::submitInner(const Action& a) {
  if (!isShowAction(a.id)) return R(false, "not a show action");
  if (a.expectRevision != 0 && a.expectRevision != rev_ && !revisionExempt(a.id)) return R(false, "stale revision (current " + std::to_string(rev_) + ")");
  if (a.trigger == Trigger::Immediate || immediateOnly(a.id)) return apply(a, false);
  // Deferred execution: Time / Beat / Bar
  Pending p; p.a = a; p.a.trigger = Trigger::Immediate; p.kind = a.trigger;
  if (a.trigger == Trigger::Time) {
    if (a.when <= evt_) return apply(p.a, false);
    p.at = a.when;
  } else {
    if (!tempoValid() || !anchorValid_) return R(false, "tempo unknown: cannot quantize to beat/bar");
    p.at = nextBoundary(evt_, a.trigger == Trigger::Bar);
    if (p.at <= evt_) return apply(p.a, false);
  }
  if (pending_.size() >= 16) return R(false, "too many scheduled actions");
  pending_.push_back(p); dirty_ = true;
  return R(true, "scheduled");
}

ActionResult ShowEngine::apply(const Action& a, bool fromPending) {
  const bool op = operatorSource(a.source);
  std::vector<int> ts;
  switch (a.id) {
    // ---- scene navigation ----
    case ActionId::NextScene: case ActionId::PrevScene: case ActionId::SelectScene: {
      if (doc_.scenes.empty()) return R(false, "no scenes");
      int t = effTarget(a);
      if (t != -1) { resolveTargets(t, ts); if (ts.empty()) return R(false, "invalid target"); }
      int idx;
      if (a.id == ActionId::SelectScene) {
        if (!isInt(a.value) || a.value < 0 || a.value >= (double)doc_.scenes.size()) return R(false, "scene index out of range");
        idx = (int)a.value;
        if (!sceneAllowed(idx)) return R(false, "strobe scene rejected: Disable Strobing is on");
      } else {
        int save = scene_;
        if (t != -1 && outSceneOverride_[(size_t)ts[0]] >= 0) scene_ = outSceneOverride_[(size_t)ts[0]];
        idx = stepScene(a.id == ActionId::NextScene ? 1 : -1);
        scene_ = save;
        if (idx < 0) return R(false, "no eligible scene (strobe disabled)");
      }
      if (op) operatorOverride("scene");
      clearManualIdle();
      if (t == -1) {
        setScene(idx, doc_.autoCfg.transition, op ? "manual" : "action");
        if (scene_ == idx) { /* unchanged or applied */ }
      } else {
        for (int i : ts) if (outSceneOverride_[(size_t)i] != idx) { outSceneOverride_[(size_t)i] = idx; dirty_ = true; emit("scene_change", json{{"scene", doc_.scenes[(size_t)idx].id}, {"output", outs_[(size_t)i].logicalId}, {"reason", "manual"}}.dump()); }
      }
      return R(true);
    }
    // ---- pause / resume ----
    case ActionId::PauseAutomation:
      if (!paused_) { paused_ = true; dirty_ = true; emit("pause", "{}"); }
      return R(true);
    case ActionId::ResumeAutomation: case ActionId::ResumeContinue: resume(false); return R(true);
    case ActionId::ResumeNext: resume(true); return R(true);
    // ---- modes ----
    case ActionId::ModeAuto:
      if (doc_.scenes.empty()) return R(false, "no scenes for Auto");
      enterMode(ShowMode::Auto); return R(true);
    case ActionId::ModeProgrammed:
      if (doc_.cues.empty()) return R(false, "no cues for Programmed");
      enterMode(ShowMode::Programmed); return R(true);
    case ActionId::ModeHybrid:
      if (doc_.scenes.empty()) return R(false, "no scenes for Hybrid");
      enterMode(ShowMode::Hybrid); return R(true);
    // ---- blackout latches (separate from pause) ----
    case ActionId::VisualBlackout:
      if (!vBlack_) { vBlack_ = true; dirty_ = true; emit("blackout", "{\"latch\":\"visual\",\"on\":true}"); }
      return R(true);
    case ActionId::LightingBlackout:
      if (!lBlack_) { lBlack_ = true; dirty_ = true; emit("blackout", "{\"latch\":\"lighting\",\"on\":true}"); }
      return R(true);
    case ActionId::EmergencyBlackout:
      if (!eBlack_) { eBlack_ = true; dirty_ = true; emit("blackout", "{\"latch\":\"emergency\",\"on\":true}"); }
      return R(true);
    case ActionId::RestoreBlackout: {   // value: 0 all latches, 1 visual, 2 lighting, 3 emergency
      if (!isInt(a.value) || a.value < 0 || a.value > 3) return R(false, "restore value must be 0..3");
      int w = (int)a.value;
      if ((w == 0 || w == 1) && vBlack_) { vBlack_ = false; dirty_ = true; emit("blackout", "{\"latch\":\"visual\",\"on\":false}"); }
      if ((w == 0 || w == 2) && lBlack_) { lBlack_ = false; dirty_ = true; emit("blackout", "{\"latch\":\"lighting\",\"on\":false}"); }
      if ((w == 0 || w == 3) && eBlack_) { eBlack_ = false; dirty_ = true; emit("blackout", "{\"latch\":\"emergency\",\"on\":false}"); }
      return R(true);
    }
    // ---- output parameters ----
    case ActionId::Brightness: case ActionId::Speed: case ActionId::Intensity: case ActionId::ColorHue: {
      double lo = 0, hi = 1;
      if (a.id == ActionId::Speed) { lo = 0.25; hi = 4; }
      if (!std::isfinite(a.value) || a.value < lo || a.value > hi) return R(false, "value out of range");
      resolveTargets(effTarget(a), ts);
      if (ts.empty()) return R(false, "invalid target");
      float v = (float)a.value;
      for (int i : ts) {
        float& f = a.id == ActionId::Brightness ? outs_[(size_t)i].brightness : a.id == ActionId::Speed ? outs_[(size_t)i].speed
                 : a.id == ActionId::Intensity ? outs_[(size_t)i].intensity : outs_[(size_t)i].hue;
        if (f != v) { f = v; dirty_ = true; }
      }
      return R(true);
    }
    case ActionId::SetTarget: {
      if (!isInt(a.value) || a.value < -1 || a.value >= (double)outs_.size()) return R(false, "target out of range");
      int t = (int)a.value;
      if (t >= 0 && outs_[(size_t)t].role == OutputRole::Controls) return R(false, "controls output is not a visual target");
      if (t != target_) { target_ = t; for (int i = 0; i < (int)outs_.size(); ++i) outs_[(size_t)i].selected = (i == t); dirty_ = true; }
      return R(true);
    }
    case ActionId::SelectBank:
      if (!isInt(a.value) || a.value < 0 || a.value > 1000) return R(false, "bank out of range");
      if (bank_ != (int)a.value) { bank_ = (int)a.value; dirty_ = true; }
      return R(true);
    // ---- tempo / key ----
    case ActionId::TapTempo: return tap(a.when != 0 ? a.when : evt_);
    case ActionId::SetBpm: {
      if (!std::isfinite(a.value) || a.value < 20 || a.value > 300) return R(false, "BPM must be 20..300");
      bool changed = !bpmManual_ || bpmSrc_ != Estimate::Manual || bpmVal_ != a.value;
      bpmManual_ = true; bpmSrc_ = Estimate::Manual; bpmVal_ = a.value; bpmSetNs_ = evt_; taps_.clear();
      if (changed) { anchorValid_ = true; anchor_ = evt_; anchorBpm_ = a.value; dirty_ = true; emit("tempo_override", json{{"bpm", a.value}, {"source", "manual"}}.dump()); }
      return R(true);
    }
    case ActionId::ReleaseBpm:
      if (bpmManual_) { bpmManual_ = false; anchorValid_ = false; taps_.clear(); dirty_ = true; emit("tempo_release", "{}"); }
      return R(true);
    case ActionId::SetKey: {
      if (!isInt(a.value) || a.value < 0 || a.value > 23) return R(false, "key must be pitch class + 12*minor (0..23)");
      int v = (int)a.value, pc = v % 12; bool minor = v >= 12;
      if (!keyManual_ || keyPc_ != pc || keyMinor_ != minor) { keyManual_ = true; keyPc_ = pc; keyMinor_ = minor; keySetNs_ = evt_; dirty_ = true; emit("key_override", json{{"key", v}}.dump()); }
      return R(true);
    }
    case ActionId::ReleaseKey:
      if (keyManual_) { keyManual_ = false; dirty_ = true; emit("key_release", "{}"); }
      return R(true);
    // ---- cues ----
    case ActionId::NextCue: case ActionId::PrevCue: case ActionId::TriggerCue: {
      if (mode_ == ShowMode::Auto) return R(false, "cues require Programmed or Hybrid mode");
      int n = (int)doc_.cues.size();
      if (n == 0) return R(false, "no cues");
      pendingCueIdx_ = -1;
      if (a.id == ActionId::TriggerCue) {
        if (!isInt(a.value) || a.value < 0 || a.value >= n) return R(false, "cue index out of range");
        if (!beginCue((int)a.value, "trigger")) return R(false, "cue unavailable (missing scene or strobe disabled)");
      } else if (a.id == ActionId::NextCue) {
        int idx = cue_ < 0 ? 0 : cue_ + 1;
        if (idx >= n) { if (doc_.cueEnd == CueListEnd::Loop) idx = 0; else return R(false, "already at last cue"); }
        if (!beginFirstAvailable(idx, 1, "next")) return R(false, "no available cue");
      } else {
        if (cue_ <= 0) return R(false, "already at first cue");
        if (!beginFirstAvailable(cue_ - 1, -1, "prev")) return R(false, "no available cue");
      }
      clearManualIdle();
      return R(true);
    }
    // ---- text ----
    case ActionId::PublishText: {
      if (!isInt(a.value) || a.value < 0 || a.value >= (double)doc_.texts.size()) return R(false, "message index out of range");
      resolveTargets(effTarget(a), ts);
      if (ts.empty()) return R(false, "invalid target");
      const TextMessage& m = doc_.texts[(size_t)a.value];
      publishTo(effTarget(a), m.id, m.text);
      return R(true);
    }
    case ActionId::ClearText:
      resolveTargets(effTarget(a), ts);
      if (ts.empty()) return R(false, "invalid target");
      publishTo(effTarget(a), "", "");
      return R(true);
    // ---- lighting arm request (the lighting module owns the actual state machine) ----
    case ActionId::ArmLighting:
      if (!armReq_) { armReq_ = true; dirty_ = true; emit("lighting_arm", "{\"armed\":true}"); }
      return R(true);
    case ActionId::DisarmLighting:
      if (armReq_) { armReq_ = false; dirty_ = true; emit("lighting_arm", "{\"armed\":false}"); }
      return R(true);
    // ---- strobe ----
    case ActionId::DisableStrobing: {
      if (a.value != 0 && a.value != 1) return R(false, "value must be 0 or 1");
      bool on = a.value == 1;
      if (on == strobeOff_) return R(true);
      strobeOff_ = on; dirty_ = true;
      if (on) {
        if (scene_ >= 0 && !sceneAllowed(scene_)) { int n = stepScene(1); if (n >= 0) setScene(n, Transition{"cut", 0}, "strobe_disabled"); }
        for (int& o : outSceneOverride_) if (o >= 0 && !sceneAllowed(o)) o = -1;
      }
      emit("strobe_policy", json{{"disabled", on}}.dump());
      return R(true);
    }
    default: break;
  }
  (void)fromPending;
  return R(false, "unhandled action");
}

// ---- snapshot ----------------------------------------------------------------------------------------
ShowSnapshot ShowEngine::snapshot() const {
  ShowSnapshot s;
  s.revision = rev_; s.mode = mode_; s.paused = paused_; s.manualOverride = override_;
  s.cueIndex = cue_; s.cueEnded = cueEnded_; s.cueWaitingForTempo = cueWaitTempo_;
  s.cueElapsedSec = cue_ >= 0 ? nsToSec(cueElapsed_) : 0;
  s.cueRemainingSec = -1;
  if (cue_ >= 0 && cue_ < (int)doc_.cues.size()) {
    const Cue& c = doc_.cues[(size_t)cue_];
    s.cueMediaRef = c.mediaRef; s.cueTextRef = c.textRef; s.cueLightingRef = c.lightingRef; s.cueMediaMissing = cueMediaMissing_;
    if (!c.duration.beats && c.duration.ms > 0) s.cueRemainingSec = std::max(0.0, nsToSec(c.duration.ms * kNsPerMs - cueElapsed_));
    else if (c.duration.beats && c.duration.beatCount > 0 && effTempo_.valid && effTempo_.value > 0) s.cueRemainingSec = std::max(0.0, (c.duration.beatCount - cueBeats_) * 60.0 / effTempo_.value);
  }
  s.sceneIndex = scene_; s.outgoingSceneIndex = outgoing_; s.transitionProgress = transProg_; s.transitionType = transType_;
  s.outputs = outs_;
  for (size_t i = 0; i < s.outputs.size(); ++i) {
    s.outputs[i].selected = (target_ == (int)i);
    s.outputs[i].independentScene = outSceneOverride_[i] >= 0;
    s.outputs[i].sceneIndex = outSceneOverride_[i] >= 0 ? outSceneOverride_[i] : scene_;
  }
  s.target = target_; s.bank = bank_;
  s.visualBlackoutLatch = vBlack_; s.lightingBlackoutLatch = lBlack_; s.emergencyBlackoutLatch = eBlack_;
  s.visualBlackedOut = vBlack_ || eBlack_; s.lightingBlackedOut = lBlack_ || eBlack_;
  s.strobeDisabled = strobeOff_;
  s.sceneStrobeSuppressed = strobeOff_ && scene_ >= 0 && scene_ < (int)doc_.scenes.size() && doc_.scenes[(size_t)scene_].strobe;
  s.lightingArmRequested = armReq_;
  s.seed = seed_;
  s.tempo = effTempo_; s.key = effKey_; s.tempoOverridden = bpmManual_; s.keyOverridden = keyManual_;
  s.audioLost = audioLost_; s.idleActive = idle_; s.idleScreen = doc_.idle.screen;
  s.draftText = draft_; s.previewText = preview_;
  s.programmedWaitBoundary = pendingCueIdx_ >= 0 || autoWaitUntil_ >= 0;
  s.pendingActions = pending_.size();
  return s;
}

} // namespace show
} // namespace ss
