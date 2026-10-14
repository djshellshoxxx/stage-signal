#include "midi/router.h"
#include <algorithm>
#include <set>

namespace ss {

// ---- route graph ---------------------------------------------------------------------------
std::string MidiRouteGraph::group(const std::string& port) const {
  std::vector<std::string> comp{port};
  for (bool grew = true; grew;) {
    grew = false;
    for (auto& p : same_) {
      bool a = std::find(comp.begin(), comp.end(), p.first) != comp.end();
      bool b = std::find(comp.begin(), comp.end(), p.second) != comp.end();
      if (a && !b) { comp.push_back(p.second); grew = true; }
      else if (b && !a) { comp.push_back(p.first); grew = true; }
    }
  }
  return *std::min_element(comp.begin(), comp.end());
}
bool MidiRouteGraph::riskLocked(const MidiRoute& r) const {
  if (r.kind == RouteKind::InputToOutput) return true;
  if (r.kind == RouteKind::ActionToOutput) {
    std::string g = group(r.outPort);
    for (auto& o : r_) if (o.kind == RouteKind::InputToAction && group(o.inPort) == g) return true;
  }
  return false;
}
void MidiRouteGraph::revalidateLocked() {
  for (auto& r : r_) if (r.enabled && !r.explicitlyEnabled && riskLocked(r)) r.enabled = false;
}
void MidiRouteGraph::declareSameDevice(const std::string& a, const std::string& b) {
  std::lock_guard<std::mutex> l(m_); same_.emplace_back(a, b); revalidateLocked();
}
Status MidiRouteGraph::addRoute(MidiRoute r) {
  std::lock_guard<std::mutex> l(m_);
  if (r.id.empty()) return Status::Fail(Err::InvalidArgument, "route id required");
  for (auto& o : r_) if (o.id == r.id) return Status::Fail(Err::InvalidArgument, "duplicate route id");
  r.explicitlyEnabled = false; r.suspended = false;
  r_.push_back(r);
  if (riskLocked(r_.back())) r_.back().enabled = false;
  revalidateLocked();
  return Status::Ok();
}
bool MidiRouteGraph::removeRoute(const std::string& id) {
  std::lock_guard<std::mutex> l(m_);
  for (size_t i = 0; i < r_.size(); ++i) if (r_[i].id == id) { r_.erase(r_.begin() + i); return true; }
  return false;
}
Status MidiRouteGraph::enable(const std::string& id, bool confirm) {
  std::lock_guard<std::mutex> l(m_);
  for (auto& r : r_) if (r.id == id) {
    bool risk = riskLocked(r);
    if (risk && !confirm) return Status::Fail(Err::PermissionDenied, "feedback-risk route requires explicit enable");
    r.enabled = true; r.explicitlyEnabled = risk; r.suspended = false; r.suspendReason.clear();
    return Status::Ok();
  }
  return Status::Fail(Err::InvalidArgument, "no such route");
}
void MidiRouteGraph::disable(const std::string& id) {
  std::lock_guard<std::mutex> l(m_);
  for (auto& r : r_) if (r.id == id) { r.enabled = false; r.explicitlyEnabled = false; }
}
bool MidiRouteGraph::suspend(const std::string& id, const std::string& why) {
  std::lock_guard<std::mutex> l(m_);
  for (auto& r : r_) if (r.id == id) { r.suspended = true; r.suspendReason = why; return true; }
  return false;
}
bool MidiRouteGraph::isActive(const std::string& id) const {
  std::lock_guard<std::mutex> l(m_);
  for (auto& r : r_) if (r.id == id) return r.enabled && !r.suspended;
  return false;
}
bool MidiRouteGraph::feedbackRisk(const std::string& id) const {
  std::lock_guard<std::mutex> l(m_);
  for (auto& r : r_) if (r.id == id) return riskLocked(r);
  return false;
}
bool MidiRouteGraph::get(const std::string& id, MidiRoute& out) const {
  std::lock_guard<std::mutex> l(m_);
  for (auto& r : r_) if (r.id == id) { out = r; return true; }
  return false;
}
std::vector<MidiRoute> MidiRouteGraph::list() const { std::lock_guard<std::mutex> l(m_); return r_; }

// ---- router --------------------------------------------------------------------------------
MidiRouter::MidiRouter(MappingTable& t, MidiRouteGraph& g, IClock& c) : table_(t), graph_(g), clock_(c) {
  q_.reserve(kQueueCap + 1); latch_.reserve(kLatchCap + 1); ports_.reserve(16); alarms_.reserve(64);
  st_.queueCapacity = kQueueCap;
}
MidiRouter::~MidiRouter() {
  std::vector<IMidiPort*> ps;
  { std::lock_guard<std::mutex> l(m_); for (auto& p : ports_) if (p.port) { ps.push_back(p.port); p.port = nullptr; } }
  for (auto* p : ps) { p->setLostCallback(nullptr); p->closeInput(); }
}
void MidiRouter::setActionSink(ActionSink s) { std::lock_guard<std::mutex> l(m_); sink_ = std::move(s); }
void MidiRouter::setAlarmCallback(std::function<void(const MidiAlarm&)> cb) { std::lock_guard<std::mutex> l(m_); alarmCb_ = std::move(cb); }
void MidiRouter::setLostForward(std::function<void(int, bool)> f) { std::lock_guard<std::mutex> l(m_); lostFwd_ = std::move(f); }
void MidiRouter::setRateCeiling(int n) { std::lock_guard<std::mutex> l(m_); rateCeiling_ = n; for (auto& p : ports_) p.rate.setCeiling(n); }
void MidiRouter::setEchoThreshold(int n) { std::lock_guard<std::mutex> l(m_); echoMax_ = std::min(22, std::max(1, n)); }

int MidiRouter::addPort(const std::string& id, const std::string& name) {
  std::lock_guard<std::mutex> l(m_);
  for (size_t i = 0; i < ports_.size(); ++i) if (ports_[i].id == id) return (int)i;
  Port p; p.id = id; p.name = name; p.route = "in:" + id; p.rate.setCeiling(rateCeiling_);
  ports_.push_back(std::move(p));
  MidiRoute r; r.id = "in:" + id; r.kind = RouteKind::InputToAction; r.inPort = id; r.enabled = true;
  graph_.addRoute(r);
  return (int)ports_.size() - 1;
}
std::string MidiRouter::routeIdForPort(int i) const {
  std::lock_guard<std::mutex> l(m_);
  return i >= 0 && (size_t)i < ports_.size() ? ports_[i].route : std::string();
}
uint32_t MidiRouter::portGeneration(int i) const {
  std::lock_guard<std::mutex> l(m_);
  return i >= 0 && (size_t)i < ports_.size() ? ports_[i].gen : 0;
}

Status MidiRouter::attach(IMidiPort& port, const MidiPortInfo& info, int* outIdx) {
  int idx = addPort(info.stableId, info.name);
  if (outIdx) *outIdx = idx;
  { std::lock_guard<std::mutex> l(m_); ports_[idx].port = &port; }
  port.setLostCallback([this, idx](bool input) {
    if (input) onPortLost(idx);
    std::function<void(int, bool)> f; { std::lock_guard<std::mutex> l(m_); f = lostFwd_; }
    if (f) f(idx, input);
  });
  Status s = port.openInput(info.stableId, [this, idx](Ns t, const uint8_t* d, size_t n) { onBytes(idx, t, d, n); });
  if (!s.ok()) onPortLost(idx);
  return s;
}
Status MidiRouter::reconnect(int idx) {
  IMidiPort* p; std::string id;
  { std::lock_guard<std::mutex> l(m_);
    if (idx < 0 || (size_t)idx >= ports_.size() || !ports_[idx].port) return Status::Fail(Err::InvalidArgument, "port not attached");
    p = ports_[idx].port; id = ports_[idx].id; }
  p->closeInput();
  Status s = p->openInput(id, [this, idx](Ns t, const uint8_t* d, size_t n) { onBytes(idx, t, d, n); });
  if (s.ok()) onPortReconnected(idx);
  return s;
}
void MidiRouter::raiseLocked(MidiAlarmKind k, int port, uint64_t count, const std::string& detail) {
  if (alarms_.size() >= 64) alarms_.erase(alarms_.begin());
  MidiAlarm a; a.kind = k; a.port = port; a.count = count; a.detail = detail;
  if (port >= 0 && (size_t)port < ports_.size()) a.portId = ports_[port].id;
  alarms_.push_back(std::move(a));
}
void MidiRouter::onPortLost(int idx) {
  std::lock_guard<std::mutex> l(m_);
  if (idx < 0 || (size_t)idx >= ports_.size()) return;
  Port& p = ports_[idx];
  if (!p.alive) return;
  p.alive = false; ++p.gen;
  auto isP = [&](const QEv& e) { return e.port == idx; };
  q_.erase(std::remove_if(q_.begin(), q_.end(), isP), q_.end());
  latch_.erase(std::remove_if(latch_.begin(), latch_.end(), isP), latch_.end());
  deb_.clear();
  table_.setPortAvailable(p.id, p.name, false);
  raiseLocked(MidiAlarmKind::PortLost, idx, 0, "input device lost; mappings unavailable");
}
void MidiRouter::onPortReconnected(int idx) {
  std::lock_guard<std::mutex> l(m_);
  if (idx < 0 || (size_t)idx >= ports_.size()) return;
  Port& p = ports_[idx];
  ++p.gen; p.alive = true; p.rate.reset();
  for (auto& s : p.sigs) s = Sig();
  auto isP = [&](const QEv& e) { return e.port == idx; };
  q_.erase(std::remove_if(q_.begin(), q_.end(), isP), q_.end());       // old generation never reaches the sink
  latch_.erase(std::remove_if(latch_.begin(), latch_.end(), isP), latch_.end());
  deb_.clear();
  table_.setPortAvailable(p.id, p.name, true);
  raiseLocked(MidiAlarmKind::PortReconnected, idx, 0, "input device reconnected");
}
void MidiRouter::checkPorts() {
  std::vector<int> lost;
  { std::lock_guard<std::mutex> l(m_);
    for (size_t i = 0; i < ports_.size(); ++i) if (ports_[i].port && ports_[i].alive && !ports_[i].port->inputAlive()) lost.push_back((int)i); }
  for (int i : lost) onPortLost(i);
}

bool MidiRouter::echoCheckLocked(Port& p, const MidiMsg& m, Ns now) {
  uint32_t key = ((uint32_t)m.type << 24) | ((uint32_t)m.channel << 16) | ((uint32_t)m.number << 8) | m.value;
  Sig* s = nullptr; Sig* oldest = &p.sigs[0];
  for (auto& x : p.sigs) {
    if (x.used && x.key == key) { s = &x; break; }
    if (!x.used || x.last < oldest->last) oldest = &x;
  }
  if (!s) { s = oldest; *s = Sig(); s->used = true; s->key = key; }
  const int cap = echoMax_ + 1;                    // trip when cap events fall inside 1 s
  s->ts[s->head] = now; s->head = (s->head + 1) % 24; if (s->n < 24) ++s->n; s->last = now;
  if (s->n >= cap) {
    Ns oldestOfWindow = s->ts[((s->head - cap) % 24 + 24) % 24];
    if (now - oldestOfWindow <= kNsPerSec) { *s = Sig(); return true; }
  }
  return false;
}

void MidiRouter::onBytes(int idx, Ns arrival, const uint8_t* d, size_t n) {
  std::lock_guard<std::mutex> l(m_);
  if (idx < 0 || (size_t)idx >= ports_.size()) return;
  Port& p = ports_[idx];
  size_t off = 0;
  while (off < n) {
    size_t used = 0; MidiMsg m;
    MidiParse r = parseMidiMessage(d + off, n - off, used, m);
    off += used ? used : n;
    ++st_.received;
    if (r == MidiParse::Realtime) { ++st_.realtime; continue; }   // clock etc.: never rate-limited/queued/suppressed
    if (r != MidiParse::Message) { ++st_.ignored; continue; }
    if (!p.alive) { ++st_.staleGeneration; continue; }
    m.arrivalNs = arrival;
    if (table_.feedLearn(p.id, p.name, m)) { ++st_.learned; continue; }
    MsgClass c = table_.classify(p.id, p.name, m);
    if (!c.mapped) { ++st_.unmapped; continue; }
    if (!c.continuous && echoCheckLocked(p, m, arrival)) {
      latched_ = true; ++st_.echoTrips;
      std::string why = "echo signature: >" + std::to_string(echoMax_) + " identical trigger events within 1 s";
      graph_.suspend(p.route, why);
      auto isP = [&](const QEv& e) { return e.port == idx; };   // pending events of the looping route are discarded
      q_.erase(std::remove_if(q_.begin(), q_.end(), isP), q_.end());
      latch_.erase(std::remove_if(latch_.begin(), latch_.end(), isP), latch_.end());
      raiseLocked(MidiAlarmKind::EchoLoop, idx, (uint64_t)echoMax_ + 1, why);
    }
    if (!graph_.isActive(p.route)) { ++st_.suspendedDropped; continue; }
    if (c.critical) {
      // Priority latch: independent of the 512 queue and of the rate ceiling; latest value per control.
      bool merged = false;
      for (auto& e : latch_) if (e.port == idx && e.msg.type == m.type && e.msg.channel == m.channel && e.msg.number == m.number) {
        e.msg = m; e.gen = p.gen; merged = true; break;
      }
      if (!merged) {
        if (latch_.size() >= kLatchCap) { latch_.erase(latch_.begin()); ++st_.loss; }
        latch_.push_back(QEv{idx, p.gen, m, false, true, false});
      }
      ++st_.latched;
      continue;
    }
    if (!p.rate.allow(arrival)) { ++st_.rateDropped; continue; }
    if (c.continuous && !c.relative) {
      bool merged = false;
      for (auto& e : q_) if (e.continuous && !e.relative && e.port == idx && e.msg.type == m.type && e.msg.channel == m.channel && e.msg.number == m.number) {
        e.msg = m; e.gen = p.gen; merged = true; break;
      }
      if (merged) { ++st_.coalescedAtIngest; continue; }
    }
    if (q_.size() >= kQueueCap) {
      size_t victim = 0; bool cc = false;
      for (size_t i = 0; i < q_.size(); ++i) if (q_[i].continuous) { victim = i; cc = true; break; }   // oldest stale CC first
      q_.erase(q_.begin() + victim);
      if (cc) ++st_.droppedStaleCc; else ++st_.droppedOldest;
      ++st_.loss;
    }
    q_.push_back(QEv{idx, p.gen, m, c.continuous, false, c.relative});
    ++st_.queued;
  }
}

namespace {
struct Out { Action a; bool cont; bool scene; Ns arrival; };
}

size_t MidiRouter::deliver(std::vector<QEv>& evs, bool ordinary) {
  if (evs.empty()) return 0;
  std::vector<std::pair<std::string, std::string>> pinfo;
  ActionSink sink;
  { std::lock_guard<std::mutex> l(m_);
    for (auto& p : ports_) pinfo.emplace_back(p.id, p.name);
    sink = sink_; }
  std::vector<Out> outs; std::vector<MidiMatch> ms;
  for (auto& e : evs) {
    ms.clear();
    table_.translate(pinfo[e.port].first, pinfo[e.port].second, e.msg, ms);
    for (auto& mm : ms) {
      mm.action.correlation = makeMidiCorrelation((uint8_t)e.port, ++seq_);
      outs.push_back(Out{mm.action, mm.continuous, mm.sceneTrigger, e.msg.arrivalNs});
    }
  }
  std::vector<bool> keep(outs.size(), true);
  uint64_t coalesced = 0, debounced = 0;
  if (ordinary) {
    std::set<uint64_t> seen;     // continuous: only the latest value per (action,target) survives
    for (size_t i = outs.size(); i-- > 0;) if (outs[i].cont) {
      uint64_t k = ((uint64_t)outs[i].a.id << 32) ^ (uint32_t)outs[i].a.target;
      if (!seen.insert(k).second) { keep[i] = false; ++coalesced; }
    }
  }
  size_t emitted = 0;
  for (size_t i = 0; i < outs.size(); ++i) {
    if (!keep[i]) continue;
    const Out& o = outs[i];
    if (ordinary && o.scene) {   // 50 ms leading-edge debounce per (action,target[,value for absolute selects])
      uint32_t key = ((uint32_t)o.a.id << 16) ^ ((uint32_t)(o.a.target + 1) << 4) ^
                     ((o.a.id == ActionId::SelectScene || o.a.id == ActionId::TriggerCue) ? (uint32_t)(int)o.a.value * 2654435761u : 0u);
      std::lock_guard<std::mutex> l(m_);
      Debounce* f = nullptr;
      for (auto& d : deb_) if (d.key == key) { f = &d; break; }
      if (f && o.arrival - f->last >= 0 && o.arrival - f->last < kDebounceNs) { ++debounced; continue; }
      if (f) f->last = o.arrival;
      else { if (deb_.size() >= 64) deb_.erase(deb_.begin()); deb_.push_back(Debounce{key, o.arrival}); }
    }
    if (sink) sink(o.a);
    ++emitted;
  }
  { std::lock_guard<std::mutex> l(m_); st_.emitted += emitted; st_.debounced += debounced; st_.coalescedAtPump += coalesced; }
  return emitted;
}

size_t MidiRouter::pumpCritical() {
  std::vector<QEv> lt;
  { std::lock_guard<std::mutex> l(m_);
    for (auto& e : latch_) { if (e.gen == ports_[e.port].gen) lt.push_back(e); else ++st_.staleGeneration; }
    latch_.clear(); }
  return deliver(lt, false);
}

size_t MidiRouter::pump() {
  std::vector<QEv> lt, qv; std::vector<MidiAlarm> al; std::function<void(const MidiAlarm&)> cb;
  { std::lock_guard<std::mutex> l(m_);
    for (auto& e : latch_) { if (e.gen == ports_[e.port].gen) lt.push_back(e); else ++st_.staleGeneration; }
    latch_.clear();
    for (auto& e : q_) { if (e.gen == ports_[e.port].gen) qv.push_back(e); else ++st_.staleGeneration; }
    q_.clear();
    if (st_.loss > lossReported_) { raiseLocked(MidiAlarmKind::InputOverflow, -1, st_.loss - lossReported_, "MIDI input events dropped"); lossReported_ = st_.loss; }
    al.swap(alarms_); cb = alarmCb_; }
  size_t n = deliver(lt, false);
  n += deliver(qv, true);
  if (cb) for (auto& a : al) cb(a);
  return n;
}

bool MidiRouter::alarmLatched() const { std::lock_guard<std::mutex> l(m_); return latched_; }
void MidiRouter::clearAlarm() { std::lock_guard<std::mutex> l(m_); latched_ = false; }
RouterStats MidiRouter::stats() const {
  std::lock_guard<std::mutex> l(m_);
  RouterStats s = st_; s.queueDepth = q_.size();
  return s;
}

} // namespace ss
