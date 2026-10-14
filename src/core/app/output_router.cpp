#include "core/app/output_router.h"
#include <algorithm>

namespace ss { namespace app {

const char* routeStateName(RouteState s) {
  switch (s) {
    case RouteState::Unassigned: return "unassigned";
    case RouteState::Active: return "active";
    case RouteState::Disconnected: return "disconnected";
    case RouteState::NeedsReassign: return "needs_reassign";
    case RouteState::Blocked: return "blocked";
    case RouteState::Virtual: return "virtual";
    case RouteState::ControlsHost: return "controls";
  }
  return "?";
}

std::string OutputRouter::identifyLabel(const DisplayInfo& d) { return "Display " + std::to_string(d.index + 1) + ": " + d.name; }

void OutputRouter::setOutputs(const std::vector<show::Output>& outs) {
  std::lock_guard<std::mutex> l(m_);
  outs_ = outs;
  rebuildLocked();
}
void OutputRouter::setAssignments(const std::map<std::string, std::string>& a) {
  std::lock_guard<std::mutex> l(m_);
  assigned_ = a; confirmed_.clear();       // identical-monitor confirmations never survive a restart
  rebuildLocked();
}
std::map<std::string, std::string> OutputRouter::assignments() const { std::lock_guard<std::mutex> l(m_); return assigned_; }
void OutputRouter::setControlsDisplay(const std::string& id) {
  std::lock_guard<std::mutex> l(m_);
  controls_ = id; controlsInferred_ = id.empty();
  rebuildLocked();
}
std::string OutputRouter::controlsDisplay() const { std::lock_guard<std::mutex> l(m_); return controls_; }
void OutputRouter::updateDisplays(const std::vector<DisplayInfo>& cur) {
  std::lock_guard<std::mutex> l(m_);
  displays_ = cur; haveDisplays_ = true;
  rebuildLocked();
}
std::vector<DisplayInfo> OutputRouter::displays() const { std::lock_guard<std::mutex> l(m_); return displays_; }

std::string OutputRouter::topoSigLocked() const {
  std::vector<std::string> v;
  for (const auto& d : displays_) v.push_back(d.stableId + "@" + std::to_string(d.x) + "," + std::to_string(d.y) + "," + std::to_string(d.w) + "x" + std::to_string(d.h));
  std::sort(v.begin(), v.end());
  std::string s; for (auto& e : v) { s += e; s += ';'; }
  return s;
}

Status OutputRouter::assign(const std::string& id, const DisplayInfo& d) {
  std::lock_guard<std::mutex> l(m_);
  const show::Output* out = nullptr;
  for (const auto& o : outs_) if (o.logicalId == id) out = &o;
  if (!out) return Status::Fail(Err::InvalidArgument, "unknown logical output '" + id + "'");
  if (d.stableId.empty()) return Status::Fail(Err::InvalidArgument, "display has no stable identity");
  bool present = false; size_t same = 0;
  for (const auto& x : displays_) { if (x.stableId == d.stableId) { ++same; if (x.x == d.x && x.y == d.y && x.w == d.w && x.h == d.h) present = true; } }
  if (!present) return Status::Fail(Err::InvalidArgument, "display is not connected");
  if (out->role != show::OutputRole::Controls && !controls_.empty() && d.stableId == controls_)
    return Status::Fail(Err::PermissionDenied, "that display hosts the operator controls; move the controls first (audience outputs never cover them)");
  for (const auto& o : outs_) {
    if (o.logicalId == id) continue;
    auto it = assigned_.find(o.logicalId);
    if (it == assigned_.end() || it->second != d.stableId) continue;
    if (same <= 1) return Status::Fail(Err::InvalidArgument, "display is already used by output '" + o.logicalId + "'");
    auto c = confirmed_.find(o.logicalId);
    if (c != confirmed_.end() && c->second.valid && c->second.x == d.x && c->second.y == d.y) return Status::Fail(Err::InvalidArgument, "display is already used by output '" + o.logicalId + "'");
  }
  assigned_[id] = d.stableId;
  if (same > 1 || d.ambiguous) { Conf c; c.stableId = d.stableId; c.x = d.x; c.y = d.y; c.w = d.w; c.h = d.h; c.topoSig = topoSigLocked(); c.valid = true; confirmed_[id] = c; }
  else confirmed_.erase(id);
  rebuildLocked();
  return Status::Ok();
}
Status OutputRouter::assignById(const std::string& id, const std::string& stableId) {
  DisplayInfo pick; size_t n = 0;
  { std::lock_guard<std::mutex> l(m_); for (const auto& d : displays_) if (d.stableId == stableId) { pick = d; ++n; } }
  if (n == 0) return Status::Fail(Err::InvalidArgument, "display is not connected");
  if (n > 1 || pick.ambiguous) return Status::Fail(Err::InvalidArgument, "identical monitors: pick the screen explicitly (use identify labels)");
  return assign(id, pick);
}
Status OutputRouter::unassign(const std::string& id) {
  std::lock_guard<std::mutex> l(m_);
  bool known = false; for (const auto& o : outs_) if (o.logicalId == id) known = true;
  if (!known) return Status::Fail(Err::InvalidArgument, "unknown logical output '" + id + "'");
  assigned_.erase(id); confirmed_.erase(id);
  rebuildLocked();
  return Status::Ok();
}
void OutputRouter::setDiagnostics(const std::string& id, bool on) { std::lock_guard<std::mutex> l(m_); diag_[id] = on; rebuildLocked(); }
bool OutputRouter::diagnostics(const std::string& id) const { std::lock_guard<std::mutex> l(m_); auto it = diag_.find(id); return it != diag_.end() && it->second; }

void OutputRouter::rebuildLocked() {
  if (controls_.empty() && haveDisplays_ && !displays_.empty()) {   // infer ONCE: the primary display hosts the controls
    const DisplayInfo* p = &displays_[0];
    for (const auto& d : displays_) if (d.primary) { p = &d; break; }
    controls_ = p->stableId; controlsInferred_ = true;
  }
  const std::string sig = topoSigLocked();
  std::vector<Slot> next;
  bool changed = next.size() != slots_.size();
  std::map<std::string, std::string> usedBy;     // "stableId@x,y" -> logicalId
  for (size_t i = 0; i < outs_.size(); ++i) {
    const show::Output& o = outs_[i];
    Slot s;
    s.desc.logicalId = o.logicalId; s.desc.outputIndex = (int)i; s.desc.role = o.role; s.desc.overlay = o.overlay;
    auto dg = diag_.find(o.logicalId); s.desc.diagnostics = dg != diag_.end() && dg->second;
    s.view.logicalId = o.logicalId; s.view.role = o.role; s.view.diagnostics = s.desc.diagnostics;
    auto as = assigned_.find(o.logicalId);
    std::string want = as != assigned_.end() ? as->second : std::string();
    const DisplayInfo* chosen = nullptr;
    RouteState st = RouteState::Unassigned; std::string note;
    if (o.role == show::OutputRole::Controls) {
      if (want.empty()) want = controls_;
      for (const auto& d : displays_) if (d.stableId == want) { chosen = &d; break; }
      if (chosen) { st = RouteState::ControlsHost; note = "hosts operator controls"; }
      else { st = haveDisplays_ ? RouteState::Disconnected : RouteState::Unassigned; note = "controls display not present; keep the controls on an available screen"; }
    } else if (want.empty()) {
      if (o.role == show::OutputRole::Program) { st = RouteState::Virtual; note = "program composition without a window (record/broadcast)"; }
      else { st = RouteState::Unassigned; note = "choose a display for this output"; }
    } else {
      std::vector<const DisplayInfo*> m;
      for (const auto& d : displays_) if (d.stableId == want) m.push_back(&d);
      if (m.empty()) {
        if (o.role == show::OutputRole::Program) { st = RouteState::Virtual; note = "display disconnected; program continues without a window"; }
        else { st = RouteState::Disconnected; note = "display disconnected; controls unaffected. Reconnect the same screen to restore this output"; }
      } else if (!controls_.empty() && want == controls_) {
        st = RouteState::Blocked; note = "this display hosts the operator controls; audience output is not shown there";
      } else {
        bool amb = m.size() > 1; for (auto* d : m) amb = amb || d->ambiguous;
        if (amb) {
          auto c = confirmed_.find(o.logicalId);
          if (c != confirmed_.end() && c->second.valid && c->second.stableId == want && c->second.topoSig == sig)
            for (auto* d : m) if (d->x == c->second.x && d->y == c->second.y && d->w == c->second.w && d->h == c->second.h) chosen = d;
          if (!chosen) {
            if (o.role == show::OutputRole::Program) { st = RouteState::Virtual; note = "identical monitors: reassign the screen; program continues without a window"; }
            else { st = RouteState::NeedsReassign; note = "identical monitors detected: output stays idle until you pick the screen again"; }
          }
        } else chosen = m[0];
        if (chosen) {
          std::string key = chosen->stableId + "@" + std::to_string(chosen->x) + "," + std::to_string(chosen->y);
          auto u = usedBy.find(key);
          if (u != usedBy.end()) { st = RouteState::Blocked; note = "display already used by output '" + u->second + "'"; chosen = nullptr; }
          else { usedBy[key] = o.logicalId; st = RouteState::Active; note = "showing on " + chosen->name; }
        }
      }
    }
    s.desc.state = st; s.view.state = st; s.view.note = note;
    if (chosen) {
      s.desc.displayId = chosen->stableId; s.desc.x = chosen->x; s.desc.y = chosen->y; s.desc.w = chosen->w; s.desc.h = chosen->h; s.desc.rotation = chosen->rotation;
      s.view.displayId = chosen->stableId; s.view.displayName = chosen->name; s.view.identifyLabel = identifyLabel(*chosen);
      s.view.x = chosen->x; s.view.y = chosen->y; s.view.w = chosen->w; s.view.h = chosen->h; s.view.rotation = chosen->rotation;
    } else if (!want.empty()) {
      s.view.displayId = want;
      for (const auto& d : displays_) if (d.stableId == want) { s.view.displayName = d.name; s.view.identifyLabel = identifyLabel(d); break; }
    }
    // generation bumps only when the binding really changed
    uint64_t g = 0; bool same = false;
    for (const auto& p : slots_) if (p.desc.logicalId == s.desc.logicalId) {
      g = p.desc.generation;
      same = p.desc.state == s.desc.state && p.desc.displayId == s.desc.displayId && p.desc.x == s.desc.x && p.desc.y == s.desc.y &&
             p.desc.w == s.desc.w && p.desc.h == s.desc.h && p.desc.rotation == s.desc.rotation && p.desc.role == s.desc.role &&
             p.desc.diagnostics == s.desc.diagnostics && p.desc.outputIndex == s.desc.outputIndex;
    }
    if (!same) { g = ++gen_; changed = true; }
    s.desc.generation = g; s.view.generation = g;
    next.push_back(std::move(s));
  }
  if (next.size() != slots_.size()) changed = true;
  slots_ = std::move(next);
  if (changed) {
    auto t = std::make_shared<RouteTable>();
    t->generation = ++gen_;
    for (const auto& s : slots_) t->routes.push_back(s.desc);
    table_ = t;
  }
}

std::shared_ptr<const RouteTable> OutputRouter::table() const { std::lock_guard<std::mutex> l(m_); return table_; }
uint64_t OutputRouter::generation() const { std::lock_guard<std::mutex> l(m_); return table_->generation; }
std::vector<OutputRouteView> OutputRouter::views() const {
  std::lock_guard<std::mutex> l(m_);
  std::vector<OutputRouteView> v;
  bool bo = blackout();
  for (const auto& s : slots_) { v.push_back(s.view); v.back().blackedOut = bo && s.desc.role != show::OutputRole::Controls && s.desc.rendersFrames(); }
  return v;
}
bool OutputRouter::finalize(const RouteDesc& r, Frame& f) const {
  if (!blackout() || r.role == show::OutputRole::Controls) return false;
  std::fill(f.px.begin(), f.px.end(), 0u);
  return true;
}

}} // namespace ss::app
