#include "lighting/engine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace ss {

static uint64_t fnv(const std::string& s) { uint64_t h = 1469598103934665603ULL; for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; } return h; }

Status LightingEngine::validateConfig(const EngineConfig& c, bool dryRun) {
  auto bad = [](const std::string& m) { return Status::Fail(Err::InvalidArgument, "lighting config: " + m); };
  if (!(c.tickHz >= 1.0 && c.tickHz <= kMaxTickHz)) return bad("tickHz must be 1..40");
  if (c.slotsPerFrame < 2 || c.slotsPerFrame > 512 || (c.slotsPerFrame & 1)) return bad("slotsPerFrame must be even 2..512");
  if (c.sacnPriority > sacn::kMaxPriority) return bad("sACN priority must be 0..200");
  if (c.protocol == LightProtocol::ArtNet || (c.protocol == LightProtocol::Sacn && !c.sacnMulticast)) {
    if (!dryRun && c.host.empty()) return bad("unicast destination host required");
    if (artnet::isBroadcastAddress(c.host) && !c.allowBroadcast) return bad("broadcast destination requires explicit allowBroadcast opt-in");
  }
  if (c.sacnMulticast && c.protocol != LightProtocol::Sacn) return bad("multicast only applies to sACN");
  return Status::Ok();
}

LightingEngine::LightingEngine(IClock& clk, const EngineConfig& cfg, IDatagramSink* sink, IUsbDmxDevice* usb, std::unique_ptr<DryRunSink> dry)
    : clock_(clk), cfg_(cfg), sink_(sink), usb_(usb), dry_(std::move(dry)) {
  dryRun_ = dry_ != nullptr;
  if (dryRun_) sink_ = dry_.get();
  period_ = (Ns)(1e9 / cfg_.tickHz);
  src_.name = cfg_.sourceName; src_.priority = cfg_.sacnPriority; src_.cid = cfg_.cid;
  bool zero = true; for (auto b : src_.cid) if (b) zero = false;
  if (zero) src_.cid = sacn::makeCid(fnv(cfg_.sourceName));
}

std::unique_ptr<LightingEngine> LightingEngine::makeLive(IClock& c, const EngineConfig& cfg, IDatagramSink& sink, Status* err) {
  Status s = validateConfig(cfg, false); if (err) *err = s; if (!s.ok()) return nullptr;
  return std::unique_ptr<LightingEngine>(new LightingEngine(c, cfg, &sink, nullptr, nullptr));
}
std::unique_ptr<LightingEngine> LightingEngine::makeLiveUsb(IClock& c, const EngineConfig& cfg, IUsbDmxDevice& dev, Status* err) {
  EngineConfig k = cfg; k.protocol = LightProtocol::UsbDmx;
  Status s = validateConfig(k, false); if (err) *err = s; if (!s.ok()) return nullptr;
  return std::unique_ptr<LightingEngine>(new LightingEngine(c, k, nullptr, &dev, nullptr));
}
std::unique_ptr<LightingEngine> LightingEngine::makeDryRun(IClock& c, const EngineConfig& cfg, Status* err) {
  Status s = validateConfig(cfg, true); if (err) *err = s; if (!s.ok()) return nullptr;
  return std::unique_ptr<LightingEngine>(new LightingEngine(c, cfg, nullptr, nullptr, std::unique_ptr<DryRunSink>(new DryRunSink())));
}

LightingEngine::~LightingEngine() { stop(); }

void LightingEngine::eventLocked(LightEvent::Kind k, const std::string& d) {
  LightEvent e; e.kind = k; e.t = clock_.nowNs(); e.detail = d;
  if (events_.size() >= 64) events_.pop_front();
  events_.push_back(e);
}

// ------------------------------------------------------------------ patch
Status LightingEngine::applyPatch(const Patch& patch, const ProfileLibrary& lib, PatchReport* report) {
  std::lock_guard<std::mutex> l(m_);
  int minU = 0, maxU = 32767;
  if (cfg_.protocol == LightProtocol::Sacn) { minU = (int)sacn::kMinUniverse; maxU = (int)sacn::kMaxUniverse; }
  else if (cfg_.protocol == LightProtocol::UsbDmx) maxU = (int)sacn::kMaxUniverse;
  PatchReport rep = patch.validate(lib, minU, maxU);
  if (rep.ok()) for (auto& e : patch.entries) {
    const FixtureProfile* p = lib.find(e.profileId);
    if (p && e.startAddress + p->footprint() - 1 > cfg_.slotsPerFrame) rep.errors.push_back("fixture '" + e.fixtureId + "' extends beyond slotsPerFrame");
  }
  if (rep.ok() && cfg_.protocol == LightProtocol::UsbDmx) {
    std::set<int> u; for (auto& e : patch.entries) u.insert(e.universe);
    if (u.size() > 1) rep.errors.push_back("USB DMX adapter carries exactly one universe");
  }
  if (report) *report = rep;
  if (!rep.ok()) return Status::Fail(Err::InvalidArgument, rep.errors.front());

  if (state_ == ArmState::Armed) disarmLocked("patch changed", true);   // uses the OLD patch for safe frames
  fix_.clear();
  std::set<int> us; for (auto& e : patch.entries) us.insert(e.universe);
  universes_.assign(us.begin(), us.end());
  for (auto& e : patch.entries) {
    Fix f; f.e = e; f.p = *lib.find(e.profileId);
    f.uidx = (int)(std::find(universes_.begin(), universes_.end(), e.universe) - universes_.begin());
    fix_.push_back(f);
  }
  order_.clear();
  for (int rank = 0; rank < 3; ++rank)   // Yielded, Unresolved, Owner (owner written last wins overlaps)
    for (size_t i = 0; i < fix_.size(); ++i) {
      Ownership o = fix_[i].e.ownership;
      int r = o == Ownership::Yielded ? 0 : (o == Ownership::Unresolved ? 1 : 2);
      if (r == rank) order_.push_back((int)i);
    }
  boxes_.clear(); work_.assign(universes_.size(), UniverseFrame()); lastResolved_ = work_;
  artSeq_.assign(universes_.size(), 0); sacnSeq_.assign(universes_.size(), 0); mcast_.assign(universes_.size(), std::string());
  for (size_t i = 0; i < universes_.size(); ++i) {
    boxes_.push_back(std::unique_ptr<Mailbox<UniverseFrame>>(new Mailbox<UniverseFrame>()));
    work_[i].universe = universes_[i]; work_[i].length = (uint16_t)cfg_.slotsPerFrame;
    if (cfg_.protocol == LightProtocol::Sacn) mcast_[i] = sacn::multicastAddress((uint32_t)universes_[i]);
  }
  auto grid = [&](std::vector<std::vector<int64_t>>& g) { g.assign(fix_.size(), std::vector<int64_t>()); for (size_t i = 0; i < fix_.size(); ++i) g[i].assign(fix_[i].p.channels.size(), -1); };
  grid(manual_); grid(cueFrom_); grid(cueTo_); grid(cueCur_); grid(audio_);
  fading_ = false; maps_.clear();
  unresolved_ = rep.unresolved(); patchOk_ = !fix_.empty();
  for (auto& w : rep.warnings) eventLocked(LightEvent::Warning, w);
  ++epoch_; issuedNonce_ = 0; ++revision_;
  return Status::Ok();
}

bool LightingEngine::findLocked(const std::string& fid, const std::string& ch, int& f, int& c) const {
  for (size_t i = 0; i < fix_.size(); ++i) if (fix_[i].e.fixtureId == fid) {
    int k = fix_[i].p.findChannel(ch); if (k < 0) return false;
    f = (int)i; c = k; return true;
  }
  return false;
}

static int64_t clampLegal(const ChannelDef& ch, int64_t v) { return std::max<int64_t>(ch.legalMin, std::min<int64_t>(ch.legalMax, v)); }

Status LightingEngine::setManual(const std::string& fid, const std::string& ch, uint32_t raw) {
  std::lock_guard<std::mutex> l(m_); int f, c;
  if (!findLocked(fid, ch, f, c)) return Status::Fail(Err::InvalidArgument, "unknown fixture/channel");
  if (raw > fix_[f].p.channels[c].maxRaw()) return Status::Fail(Err::InvalidArgument, "value exceeds channel width");
  manual_[f][c] = clampLegal(fix_[f].p.channels[c], raw); return Status::Ok();
}
void LightingEngine::clearManual(const std::string& fid, const std::string& ch) {
  std::lock_guard<std::mutex> l(m_); int f, c; if (findLocked(fid, ch, f, c)) manual_[f][c] = -1;
}
void LightingEngine::clearAllManual() {
  std::lock_guard<std::mutex> l(m_); for (auto& v : manual_) std::fill(v.begin(), v.end(), -1);
}

Status LightingEngine::setCue(const Cue& cue) {
  std::lock_guard<std::mutex> l(m_);
  std::vector<std::pair<std::pair<int, int>, int64_t>> tgt;
  for (auto& v : cue.values) {
    int f, c; if (!findLocked(v.fixtureId, v.channel, f, c)) return Status::Fail(Err::InvalidArgument, "cue references unknown fixture/channel");
    if (v.value > fix_[f].p.channels[c].maxRaw()) return Status::Fail(Err::InvalidArgument, "cue value exceeds channel width");
    tgt.push_back(std::make_pair(std::make_pair(f, c), clampLegal(fix_[f].p.channels[c], v.value)));
  }
  Ns now = clock_.nowNs();
  updateCueLocked(now);                                  // cueCur_ = what is currently shown by the cue layer
  for (size_t f = 0; f < fix_.size(); ++f) for (size_t c = 0; c < fix_[f].p.channels.size(); ++c) { cueFrom_[f][c] = cueCur_[f][c]; cueTo_[f][c] = -1; }
  for (auto& t : tgt) cueTo_[t.first.first][t.first.second] = t.second;
  for (size_t f = 0; f < fix_.size(); ++f) for (size_t c = 0; c < fix_[f].p.channels.size(); ++c)
    if (cueTo_[f][c] >= 0 && cueFrom_[f][c] < 0) cueFrom_[f][c] = fix_[f].p.channels[c].defaultValue;
  fadeStart_ = now; fadeDur_ = cue.fadeNs; fading_ = cue.fadeNs > 0;
  ++revision_; return Status::Ok();
}
void LightingEngine::clearCue() {
  std::lock_guard<std::mutex> l(m_);
  for (size_t f = 0; f < fix_.size(); ++f) { std::fill(cueTo_[f].begin(), cueTo_[f].end(), -1); std::fill(cueCur_[f].begin(), cueCur_[f].end(), -1); }
  fading_ = false;
}

Status LightingEngine::setAudioMappings(const std::vector<AudioMapping>& ms) {
  std::lock_guard<std::mutex> l(m_); std::vector<MapIdx> out;
  for (auto& m : ms) { int f, c; if (!findLocked(m.fixtureId, m.channel, f, c)) return Status::Fail(Err::InvalidArgument, "audio mapping references unknown fixture/channel"); MapIdx x; x.f = f; x.c = c; x.m = m; out.push_back(x); }
  maps_ = out; return Status::Ok();
}

void LightingEngine::submitFeatures(const FeatureSnapshot& s) { features_.put(s); featuresAt_.store(clock_.nowNs()); }

void LightingEngine::setLightingBlackout(bool on) { lblackout_.store(on); ++revision_; }
void LightingEngine::setEmergencyBlackout(bool on) { emergency_.store(on); ++revision_; }
void LightingEngine::setStrobeDisabled(bool on) { nostrobe_.store(on); ++revision_; }

ActionResult LightingEngine::handleAction(const Action& a) {
  ActionResult r;
  auto done = [&]() { r.accepted = true; r.revision = revision_.load(); return r; };
  auto deny = [&](const char* why) { r.accepted = false; r.reason = why; r.revision = revision_.load(); return r; };
  bool human = a.source == ActionSource::UI || a.source == ActionSource::Keyboard || a.source == ActionSource::Midi;
  switch (a.id) {
    case ActionId::LightingBlackout: setLightingBlackout(a.value > 0.5); return done();
    case ActionId::EmergencyBlackout: setEmergencyBlackout(true); return done();
    case ActionId::RestoreBlackout:
      if (a.source == ActionSource::Show || a.source == ActionSource::System) return deny("blackout can only be restored by an operator");
      lblackout_.store(false); emergency_.store(false); ++revision_; return done();
    case ActionId::DisableStrobing: setStrobeDisabled(a.value > 0.5); return done();
    case ActionId::ArmLighting: {
      if (!human) return deny("arming requires a local operator action");
      ArmToken t; Status s = requestArmToken(t); if (s.ok()) s = arm(t);
      if (!s.ok()) return deny(s.detail.c_str());
      return done();
    }
    case ActionId::DisarmLighting: disarm(); return done();
    default: return deny("not a lighting action");
  }
}

// ------------------------------------------------------------------ arming
Status LightingEngine::requestArmToken(ArmToken& out) {
  std::lock_guard<std::mutex> l(m_);
  if (!transportOpen_) return Status::Fail(Err::TransportFailure, "transport not open");
  if (faulted_) return Status::Fail(Err::TransportFailure, "transport faulted; reconnect first");
  if (!patchOk_) return Status::Fail(Err::InvalidArgument, "nothing patched");
  if (unresolved_) return Status::Fail(Err::InvalidArgument, "unresolved patch conflicts; resolve ownership first");
  out.epoch = epoch_; out.nonce = ++nonceCtr_; issuedNonce_ = out.nonce; return Status::Ok();
}

Status LightingEngine::arm(const ArmToken& t) {
  std::lock_guard<std::mutex> l(m_);
  if (state_ == ArmState::Armed) return Status::Ok();
  if (!t.valid() || t.nonce != issuedNonce_ || t.epoch != epoch_) return Status::Fail(Err::PermissionDenied, "arm token invalid or stale; request a new one");
  if (!transportOpen_ || faulted_) return Status::Fail(Err::TransportFailure, "transport not ready");
  if (!patchOk_ || unresolved_) return Status::Fail(Err::InvalidArgument, "patch not armable");
  issuedNonce_ = 0; state_ = ArmState::Armed; ++revision_;
  eventLocked(LightEvent::Armed, "armed by operator token"); return Status::Ok();
}

void LightingEngine::disarm() { std::lock_guard<std::mutex> l(m_); disarmLocked("operator disarm", true); }

void LightingEngine::disarmLocked(const char* why, bool graceful) {
  if (state_ == ArmState::Armed) {
    if (graceful && transportOpen_ && !faulted_) sendSafeAndTerminateLocked();
    state_ = ArmState::Disarmed; ++revision_;
    eventLocked(LightEvent::Disarmed, why);
  }
  ++epoch_; issuedNonce_ = 0;
}

void LightingEngine::faultLocked(const Status& st) {
  bool was = state_ == ArmState::Armed;
  state_ = ArmState::Disarmed; faulted_ = true; ++faults_; lastFault_ = st; ++epoch_; issuedNonce_ = 0; ++revision_;
  eventLocked(LightEvent::Fault, std::string(was ? "transport fault while armed -> Disarmed: " : "transport fault: ") + st.detail);
}

void LightingEngine::sendSafeAndTerminateLocked() {
  std::vector<UniverseFrame> safe(universes_.size());
  for (size_t i = 0; i < safe.size(); ++i) { safe[i].universe = universes_[i]; safe[i].length = (uint16_t)cfg_.slotsPerFrame; }
  resolveLocked(true, true, safe);
  for (int rep = 0; rep < sacn::kTerminationRepeats; ++rep)
    for (size_t i = 0; i < safe.size(); ++i) {
      Status s = sendFrameLocked(i, safe[i].data, true);
      if (!s.ok()) { eventLocked(LightEvent::Fault, "safe frame send failed: " + s.detail); return; }
      ++framesSent_;
    }
}

// ------------------------------------------------------------------ transport
Status LightingEngine::openLocked() {
  if (usb_) { if (!usb_->isOpen()) return Status::Fail(Err::DeviceLost, "USB DMX device not open"); return Status::Ok(); }
  if (transportOpen_) sink_->close();
  return sink_->open(cfg_.nicAddr, cfg_.allowBroadcast);
}

Status LightingEngine::reconnect() {
  std::lock_guard<std::mutex> l(m_);
  state_ = ArmState::Disarmed;                // never auto-rearm
  Status s = openLocked();
  if (!s.ok()) { transportOpen_ = false; lastFault_ = s; return s; }
  bool wasFault = faulted_;
  transportOpen_ = true; faulted_ = false; ++epoch_; issuedNonce_ = 0;
  eventLocked(LightEvent::Reconnected, wasFault ? "transport reopened; remains Disarmed until explicit Arm" : "transport opened (Disarmed)");
  return Status::Ok();
}
Status LightingEngine::openTransport() { return reconnect(); }

void LightingEngine::notifyTransportLost(const std::string& detail) {
  std::lock_guard<std::mutex> l(m_);
  transportOpen_ = false; faultLocked(Status::Fail(Err::DeviceLost, detail));
}

Status LightingEngine::sendFrameLocked(size_t i, const uint8_t* data, bool terminate) {
  size_t n = (size_t)cfg_.slotsPerFrame;
  const std::string& host = cfg_.host.empty() ? std::string("dry-run") : cfg_.host;
  switch (cfg_.protocol) {
    case LightProtocol::ArtNet: {
      artnet::DmxParams p; p.portAddress = (uint16_t)universes_[i]; p.physical = cfg_.artnetPhysical;
      artSeq_[i] = artnet::nextSequence(artSeq_[i]); p.sequence = artSeq_[i];
      size_t len = artnet::buildArtDmx(p, data, n, pkt_.data(), pkt_.size());
      if (!len) return Status::Fail(Err::InternalFault, "ArtDmx build failed");
      return sink_->sendTo(host, artnet::kUdpPort, pkt_.data(), len);
    }
    case LightProtocol::Sacn: {
      sacn::DataParams p; p.universe = (uint16_t)universes_[i]; p.sequence = sacnSeq_[i]++; p.options = terminate ? sacn::kOptTerminated : 0;
      size_t len = sacn::buildDataPacket(src_, p, data, n, pkt_.data(), pkt_.size());
      if (!len) return Status::Fail(Err::InternalFault, "sACN build failed");
      return sink_->sendTo(cfg_.sacnMulticast ? mcast_[i] : host, sacn::kUdpPort, pkt_.data(), len);
    }
    case LightProtocol::UsbDmx: {
      if (usb_) return usb_->sendDmx(data, n);
      std::vector<uint8_t> b; if (!enttec::buildSendDmxPacket(data, n, b)) return Status::Fail(Err::InternalFault, "USB frame build failed");
      return sink_->sendTo("usb-dmx", 0, b.data(), b.size());
    }
  }
  return Status::Fail(Err::InternalFault, "bad protocol");
}

void LightingEngine::transmitLocked() {
  for (size_t i = 0; i < universes_.size(); ++i) {
    UniverseFrame f; if (!boxes_[i]->get(f)) continue;
    Status s = sendFrameLocked(i, f.data, false);
    if (!s.ok()) { faultLocked(s); return; }
    ++framesSent_;
  }
}

// ------------------------------------------------------------------ resolve
void LightingEngine::updateCueLocked(Ns now) {
  bool done = false;
  if (fading_ && now - fadeStart_ >= fadeDur_) done = true;
  for (size_t f = 0; f < fix_.size(); ++f) for (size_t c = 0; c < fix_[f].p.channels.size(); ++c) {
    int64_t to = cueTo_[f][c];
    if (to < 0) { cueCur_[f][c] = -1; continue; }
    if (!fading_) { cueCur_[f][c] = to; continue; }
    int64_t from = cueFrom_[f][c] < 0 ? to : cueFrom_[f][c];
    cueCur_[f][c] = fadeValue(fix_[f].p.channels[c], (uint32_t)from, (uint32_t)to, now - fadeStart_, fadeDur_);
  }
  if (done) fading_ = false;
}

void LightingEngine::audioLocked(Ns now) {
  for (auto& v : audio_) std::fill(v.begin(), v.end(), -1);
  Ns at = featuresAt_.load();
  if (maps_.empty() || at < 0 || now - at > cfg_.audioStaleNs) return;
  FeatureSnapshot s; if (!features_.get(s)) return;
  for (auto& mi : maps_) {
    float x = 0;
    switch (mi.m.source) {
      case AudioSource::RmsDb: x = s.rmsDbMix; break;
      case AudioSource::BandLow: x = s.band[0]; break;
      case AudioSource::BandMid: x = s.band[1]; break;
      case AudioSource::BandHigh: x = s.band[2]; break;
      case AudioSource::Onset: x = s.onsetStrength; break;
    }
    float span = mi.m.inHi - mi.m.inLo, t = span != 0 ? (x - mi.m.inLo) / span : (x >= mi.m.inHi ? 1.0f : 0.0f);
    if (!(t > 0)) t = 0;
    if (t > 1) t = 1;
    double v = (double)mi.m.outLo + ((double)mi.m.outHi - (double)mi.m.outLo) * t;
    audio_[mi.f][mi.c] = clampLegal(fix_[mi.f].p.channels[mi.c], (int64_t)std::floor(v + 0.5));
  }
}

void LightingEngine::resolveLocked(bool blackout, bool noStrobe, std::vector<UniverseFrame>& out) {
  for (auto& u : out) std::fill(u.data, u.data + 512, (uint8_t)0);
  for (int fi : order_) {
    const Fix& fx = fix_[(size_t)fi]; UniverseFrame& u = out[(size_t)fx.uidx];
    uint8_t* base = u.data + (fx.e.startAddress - 1);
    for (size_t c = 0; c < fx.p.channels.size(); ++c) {
      const ChannelDef& ch = fx.p.channels[c];
      int64_t v;
      if (blackout) v = ch.blackout();
      else {
        v = ch.defaultValue;
        if (audio_[fi][c] >= 0) v = audio_[fi][c];
        if (cueCur_[fi][c] >= 0) v = cueCur_[fi][c];
        if (manual_[fi][c] >= 0) v = manual_[fi][c];
        if (noStrobe && ch.isStrobe()) v = ch.safeValue;      // REQ-S12
      }
      encodeChannel(ch, (uint32_t)v, base);
    }
  }
}

void LightingEngine::tickLocked(Ns now) {
  ++ticks_;
  bool emer = emergency_.load(), lb = lblackout_.load(), ns = nostrobe_.load();
  if (emer != prevEmer_) { eventLocked(LightEvent::Blackout, emer ? "emergency blackout ON" : "emergency blackout off"); prevEmer_ = emer; }
  if (lb != prevLb_) { eventLocked(LightEvent::Blackout, lb ? "lighting blackout ON" : "lighting blackout off"); prevLb_ = lb; }
  if (ns != prevNs_) { eventLocked(LightEvent::Strobe, ns ? "strobing disabled" : "strobing allowed"); prevNs_ = ns; }
  updateCueLocked(now); audioLocked(now);
  resolveLocked(emer || lb, ns, work_);
  for (size_t i = 0; i < work_.size(); ++i) { work_[i].tick = ticks_; boxes_[i]->put(work_[i]); lastResolved_[i] = work_[i]; }
  if (state_ == ArmState::Armed) transmitLocked();
}

int LightingEngine::pump() {
  std::lock_guard<std::mutex> l(m_);
  Ns now = clock_.nowNs();
  if (!started_) { started_ = true; nextTick_ = now; }
  if (now - nextTick_ > 3 * period_) {          // far behind: coalesce stale ticks, resample newest state once
    skipped_ += (uint64_t)((now - nextTick_) / period_);
    tickLocked(now); nextTick_ = now + period_; return 1;
  }
  int n = 0;
  while (nextTick_ <= now) { tickLocked(now); nextTick_ += period_; ++n; }
  return n;
}

Status LightingEngine::start() {
  {
    std::lock_guard<std::mutex> l(m_);
    if (running_) return Status::Ok();
    if (!transportOpen_) { Status s = openLocked(); if (!s.ok()) return s; transportOpen_ = true; faulted_ = false; }
    running_ = true; stopFlag_ = false;
  }
  th_ = std::thread([this] { while (!stopFlag_.load()) { pump(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } });
  return Status::Ok();
}

void LightingEngine::stop() {
  stopFlag_.store(true);
  if (th_.joinable()) th_.join();
  std::lock_guard<std::mutex> l(m_);
  running_ = false; started_ = false;
  if (state_ == ArmState::Armed) disarmLocked("graceful stop", true);
  if (transportOpen_ && sink_) sink_->close();
  transportOpen_ = false;
}

// ------------------------------------------------------------------ observation
EngineStatus LightingEngine::status() const {
  std::lock_guard<std::mutex> l(m_); EngineStatus s;
  s.state = state_; s.dryRun = dryRun_; s.transportOpen = transportOpen_; s.faulted = faulted_;
  s.lightingBlackout = lblackout_.load(); s.emergencyBlackout = emergency_.load(); s.strobeDisabled = nostrobe_.load();
  s.unresolvedConflicts = unresolved_; s.fixtures = fix_.size(); s.ticks = ticks_; s.skippedTicks = skipped_;
  s.framesSent = framesSent_; s.faults = faults_; s.lastFault = lastFault_; return s;
}
std::vector<LightEvent> LightingEngine::drainEvents() {
  std::lock_guard<std::mutex> l(m_); std::vector<LightEvent> v(events_.begin(), events_.end()); events_.clear(); return v;
}
bool LightingEngine::latestFrame(int universe, UniverseFrame& out) const {
  std::lock_guard<std::mutex> l(m_);
  for (size_t i = 0; i < universes_.size(); ++i) if (universes_[i] == universe) return boxes_[i]->get(out);
  return false;
}
std::vector<int> LightingEngine::universes() const { std::lock_guard<std::mutex> l(m_); return universes_; }
bool LightingEngine::resolvedValue(const std::string& fid, const std::string& ch, uint32_t& out) const {
  std::lock_guard<std::mutex> l(m_); int f, c;
  if (!findLocked(fid, ch, f, c) || ticks_ == 0) return false;
  out = decodeChannel(fix_[f].p.channels[c], lastResolved_[(size_t)fix_[f].uidx].data + (fix_[f].e.startAddress - 1)); return true;
}

} // namespace ss
