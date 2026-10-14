#include "midi/output.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace ss {

static const size_t kJitterRing = 32768;   // > 10 min of pulses at 120 BPM (28,800)

MidiOutputScheduler::MidiOutputScheduler(IClock& c, IMidiPort& p, MidiOutConfig cfg)
    : clock_(c), port_(p), cfg_(cfg), rate_(cfg.maxMsgsPerSec) {
  q_.reserve(cfg.queueCap + 64);
}
MidiOutputScheduler::~MidiOutputScheduler() { stop(); }

void MidiOutputScheduler::setConfig(const MidiOutConfig& c) { std::lock_guard<std::mutex> l(m_); cfg_ = c; rate_.setCeiling(c.maxMsgsPerSec); }
MidiOutConfig MidiOutputScheduler::config() const { std::lock_guard<std::mutex> l(m_); return cfg_; }

static bool isNoteOff(const std::vector<uint8_t>& b) {
  return b.size() == 3 && (((b[0] & 0xF0) == 0x80) || (((b[0] & 0xF0) == 0x90) && b[2] == 0));
}
static bool isNoteOn(const std::vector<uint8_t>& b) { return b.size() == 3 && (b[0] & 0xF0) == 0x90 && b[2] != 0; }

Status MidiOutputScheduler::validate(const MidiOutMessage& m) const {
  const auto& b = m.bytes;
  if (b.empty()) return Status::Fail(Err::InvalidArgument, "empty MIDI message");
  if (m.origin == MidiOrigin::MidiInput) return Status::Fail(Err::PermissionDenied, "MIDI-originated action may not emit MIDI (feedback protection)");
  uint8_t s = b[0];
  if (s < 0x80) return Status::Fail(Err::InvalidArgument, "missing status byte");
  if (s == 0xF0) {
    if (!cfg_.sysexEnabled) return Status::Fail(Err::PermissionDenied, "SysEx disabled");
    if (b.size() > cfg_.maxSysexBytes) return Status::Fail(Err::InvalidArgument, "SysEx exceeds size limit");
    if (b.size() < 2 || b.back() != 0xF7) return Status::Fail(Err::InvalidArgument, "SysEx not terminated");
    for (size_t i = 1; i + 1 < b.size(); ++i) if (b[i] & 0x80) return Status::Fail(Err::InvalidArgument, "SysEx data byte >= 0x80");
    return Status::Ok();
  }
  if (s == 0xFA || s == 0xFB || s == 0xFC) {
    if (b.size() != 1) return Status::Fail(Err::InvalidArgument, "bad transport message");
    bool ok = s == 0xFA ? cfg_.allowStart : s == 0xFC ? cfg_.allowStop : cfg_.allowContinue;
    return ok ? Status::Ok() : Status::Fail(Err::PermissionDenied, "transport message not enabled");
  }
  if (s >= 0xF0) return Status::Fail(Err::InvalidArgument, "unsupported system message (clock uses the generator)");
  size_t need = ((s & 0xF0) == 0xC0 || (s & 0xF0) == 0xD0) ? 2 : 3;
  if (b.size() != need) return Status::Fail(Err::InvalidArgument, "bad message length");
  for (size_t i = 1; i < b.size(); ++i) if (b[i] & 0x80) return Status::Fail(Err::InvalidArgument, "data byte >= 0x80");
  return Status::Ok();
}

Status MidiOutputScheduler::enqueueLocked(MidiOutMessage&& m, bool priority) {
  if (!portUp_) return Status::Fail(Err::DeviceLost, "MIDI output unavailable");
  if (m.generation != 0 && m.generation != gen_) { ++st_.staleGeneration; return Status::Fail(Err::Cancelled, "stale generation"); }
  if (m.armToken != 0 && m.armToken != arm_) { ++st_.staleGeneration; return Status::Fail(Err::Cancelled, "arm token mismatch"); }
  if (!priority && q_.size() >= cfg_.queueCap) { ++st_.queueFull; return Status::Fail(Err::ResourceExhausted, "MIDI output queue full"); }
  Ns now = clock_.nowNs();
  Pending p; p.deadline = priority ? INT64_MIN : (m.deadline <= 0 ? now : m.deadline); p.seq = ++seq_; p.priority = priority; p.msg = std::move(m);
  auto it = std::upper_bound(q_.begin(), q_.end(), p, [](const Pending& a, const Pending& b) { return a.deadline != b.deadline ? a.deadline < b.deadline : a.seq < b.seq; });
  q_.insert(it, std::move(p));
  return Status::Ok();
}

Status MidiOutputScheduler::submit(MidiOutMessage m) {
  Status r;
  {
    std::lock_guard<std::mutex> l(m_);
    r = validate(m);
    if (r.ok()) r = enqueueLocked(std::move(m), false);
    else ++st_.rejected;
  }
  cv_.notify_all();
  return r;
}
static Status chk(int ch, int a, int b) {
  return (ch < 0 || ch > 15 || a < 0 || a > 127 || b < 0 || b > 127) ? Status::Fail(Err::InvalidArgument, "MIDI argument out of range") : Status::Ok();
}
static MidiOutMessage mk(std::vector<uint8_t> b, Ns dl, MidiOrigin o) { MidiOutMessage m; m.bytes = std::move(b); m.deadline = dl; m.origin = o; return m; }
Status MidiOutputScheduler::noteOn(int ch, int n, int v, Ns dl, MidiOrigin o) {
  Status s = chk(ch, n, v); if (!s.ok()) return s;
  return submit(mk({(uint8_t)(0x90 | ch), (uint8_t)n, (uint8_t)v}, dl, o));
}
Status MidiOutputScheduler::noteOff(int ch, int n, Ns dl, MidiOrigin o) {
  Status s = chk(ch, n, 0); if (!s.ok()) return s;
  return submit(mk({(uint8_t)(0x80 | ch), (uint8_t)n, 0}, dl, o));
}
Status MidiOutputScheduler::cc(int ch, int c, int v, Ns dl, MidiOrigin o) {
  Status s = chk(ch, c, v); if (!s.ok()) return s;
  return submit(mk({(uint8_t)(0xB0 | ch), (uint8_t)c, (uint8_t)v}, dl, o));
}
Status MidiOutputScheduler::programChange(int ch, int p, Ns dl, MidiOrigin o) {
  Status s = chk(ch, p, 0); if (!s.ok()) return s;
  return submit(mk({(uint8_t)(0xC0 | ch), (uint8_t)p}, dl, o));
}
Status MidiOutputScheduler::sysex(const std::vector<uint8_t>& b, Ns dl, MidiOrigin o) { return submit(mk(b, dl, o)); }
Status MidiOutputScheduler::sendStart(Ns dl) { return submit(mk({0xFA}, dl, MidiOrigin::Operator)); }
Status MidiOutputScheduler::sendStop(Ns dl) { return submit(mk({0xFC}, dl, MidiOrigin::Operator)); }
Status MidiOutputScheduler::sendContinue(Ns dl) { return submit(mk({0xFB}, dl, MidiOrigin::Operator)); }

void MidiOutputScheduler::setTempoSource(std::function<double()> f) { std::lock_guard<std::mutex> l(m_); tempo_ = std::move(f); }
void MidiOutputScheduler::setFixedTempo(double bpm) { std::lock_guard<std::mutex> l(m_); fixedBpm_ = bpm; tempo_ = nullptr; }
void MidiOutputScheduler::setClockEnabled(bool on) {
  { std::lock_guard<std::mutex> l(m_); clockOn_ = on; if (!on) { clockRunning_ = false; lastPulseValid_ = false; } }
  cv_.notify_all();
}
bool MidiOutputScheduler::clockEnabled() const { std::lock_guard<std::mutex> l(m_); return clockOn_; }

size_t MidiOutputScheduler::ownedNoteCount() const { std::lock_guard<std::mutex> l(m_); return owned_.size(); }
size_t MidiOutputScheduler::releaseOwnedNotes() {
  size_t n = 0;
  { std::lock_guard<std::mutex> l(m_);
    for (auto& kv : owned_) {
      MidiOutMessage m; m.bytes = {(uint8_t)(0x80 | (kv.first >> 7)), (uint8_t)(kv.first & 0x7F), 0}; m.origin = MidiOrigin::System;
      if (enqueueLocked(std::move(m), true).ok()) ++n;
    } }
  cv_.notify_all();
  return n;
}
uint32_t MidiOutputScheduler::generation() const { std::lock_guard<std::mutex> l(m_); return gen_; }
void MidiOutputScheduler::arm(uint64_t t) { std::lock_guard<std::mutex> l(m_); arm_ = t; }
void MidiOutputScheduler::disarm() { std::lock_guard<std::mutex> l(m_); arm_ = 0; q_.erase(std::remove_if(q_.begin(), q_.end(), [](const Pending& p) { return p.msg.armToken != 0; }), q_.end()); }
void MidiOutputScheduler::notePortLost() {
  std::lock_guard<std::mutex> l(m_);
  portUp_ = false; ++gen_; q_.clear(); owned_.clear(); clockRunning_ = false; lastPulseValid_ = false; rate_.reset();
}
void MidiOutputScheduler::notePortRestored() {
  { std::lock_guard<std::mutex> l(m_); portUp_ = true; ++gen_; q_.clear(); clockRunning_ = false; lastPulseValid_ = false; }
  cv_.notify_all();
}

Ns MidiOutputScheduler::step() {
  const Ns now = clock_.nowNs();
  std::function<double()> tempo; double fixed; bool clk;
  { std::lock_guard<std::mutex> l(m_); tempo = tempo_; fixed = fixedBpm_; clk = clockOn_ && portUp_; }
  double bpm = clk ? (tempo ? tempo() : fixed) : 0.0;

  std::vector<Due> due;
  { std::lock_guard<std::mutex> l(m_);
    if (!portUp_) return kNoDeadline;
    size_t k = 0;
    while (k < q_.size() && q_[k].deadline <= now) {
      Pending& p = q_[k];
      if (p.msg.generation != 0 && p.msg.generation != gen_) ++st_.staleGeneration;
      else if (p.msg.armToken != 0 && p.msg.armToken != arm_) ++st_.staleGeneration;
      else { Due d; d.deadline = p.priority ? now : p.deadline; d.msg = std::move(p.msg); due.push_back(std::move(d)); }
      ++k;
    }
    q_.erase(q_.begin(), q_.begin() + k);

    if (clockOn_ && std::isfinite(bpm) && bpm >= 1.0 && bpm <= 400.0) {
      Ns period = (Ns)(60.0e9 / (bpm * 24.0));
      if (!clockRunning_) { clockRunning_ = true; nextPulse_ = now; lastPulseValid_ = false; gap_ = false; }
      if (nextPulse_ <= now) {
        Ns late = now - nextPulse_;
        if (late >= period) {                        // worker stalled: skip missed pulses instead of bursting
          Ns skipped = late / period;
          nextPulse_ += skipped * period; st_.clockSkipped += (uint64_t)skipped; gap_ = true;
        }
        Due d; d.pulse = true; d.deadline = nextPulse_; d.period = period; d.gapBefore = gap_; d.msg.bytes = {0xF8};
        d.msg.origin = MidiOrigin::System;
        gap_ = false;
        due.push_back(std::move(d));
        nextPulse_ += period;
      }
    } else if (clockRunning_) { clockRunning_ = false; lastPulseValid_ = false; }   // tempo invalid: pause, resync later

    std::stable_sort(due.begin(), due.end(), [](const Due& a, const Due& b) { return a.deadline < b.deadline; });
    // stale-deadline coalescing: an older stale CC is superseded by a later value for the same controller
    std::set<uint32_t> seen;
    for (size_t i = due.size(); i-- > 0;) {
      const auto& b = due[i].msg.bytes;
      if (b.size() == 3 && (b[0] & 0xF0) == 0xB0) {
        uint32_t key = ((uint32_t)b[0] << 8) | b[1];
        bool stale = now - due[i].deadline > cfg_.staleNs;
        if (!seen.insert(key).second && stale) { due[i].msg.bytes.clear(); ++st_.coalesced; }
      }
    }
  }

  bool lost = false;
  for (auto& d : due) {
    const auto& b = d.msg.bytes;
    if (b.empty()) continue;
    bool exempt = b[0] >= 0xF8 || isNoteOff(b);
    { std::lock_guard<std::mutex> l(m_);
      if (!exempt && !rate_.allow(now)) { ++st_.rateDropped; continue; } }
    Ns t = clock_.nowNs();
    Status s = port_.send(b.data(), b.size());
    std::lock_guard<std::mutex> l(m_);
    if (!s.ok()) { ++st_.sendFailures; if (s.code == Err::DeviceLost) lost = true; continue; }
    ++st_.sent;
    if (d.pulse) {
      ++st_.clockPulses;
      if (lastPulseValid_ && !d.gapBefore) {
        double err = std::fabs((double)(t - lastPulseSend_) - (double)d.period);
        if (jit_.size() < kJitterRing) jit_.push_back((float)err); else { jit_[jitHead_] = (float)err; jitHead_ = (jitHead_ + 1) % kJitterRing; }
        if (err > jitMax_) jitMax_ = err;
      }
      lastPulseSend_ = t; lastPulseValid_ = true;
    } else if (b[0] == 0xF0) ++st_.sysex;
    else if (b[0] >= 0xF0) ++st_.transport;
    else if (isNoteOn(b)) { ++st_.notes; ++owned_[(uint16_t)(((b[0] & 0x0F) << 7) | b[1])]; }
    else if (isNoteOff(b)) {
      ++st_.notes;
      auto it = owned_.find((uint16_t)(((b[0] & 0x0F) << 7) | b[1]));
      if (it != owned_.end() && --it->second <= 0) owned_.erase(it);
    } else if ((b[0] & 0xF0) == 0xB0) ++st_.cc;
  }
  if (lost) notePortLost();

  std::lock_guard<std::mutex> l(m_);
  Ns next = kNoDeadline;
  if (!q_.empty()) next = q_.front().deadline;
  if (clockRunning_ && nextPulse_ < next) next = nextPulse_;
  return next;
}

void MidiOutputScheduler::run() {
  for (;;) {
    Ns next = step();
    std::unique_lock<std::mutex> lk(m_);
    if (stopFlag_) return;
    Ns delta = next == kNoDeadline ? 20 * kNsPerMs : next - clock_.nowNs();
    if (delta > 2 * kNsPerMs) cv_.wait_for(lk, std::chrono::nanoseconds(std::min<Ns>(delta - kNsPerMs, 20 * kNsPerMs)));
    else if (delta > 0) { lk.unlock(); std::this_thread::yield(); }
  }
}
void MidiOutputScheduler::start() {
  std::lock_guard<std::mutex> lf(life_);
  if (running_) return;
  { std::lock_guard<std::mutex> l(m_); stopFlag_ = false; }
  running_ = true;
  th_ = std::thread([this] { run(); });
}
bool MidiOutputScheduler::running() const { return running_; }
void MidiOutputScheduler::stop() {
  std::lock_guard<std::mutex> lf(life_);
  if (running_) {
    { std::lock_guard<std::mutex> l(m_); stopFlag_ = true; }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    running_ = false;
  }
  // Release only notes we sent (never All Notes Off), then quiesce.
  std::vector<uint16_t> keys; bool up;
  { std::lock_guard<std::mutex> l(m_);
    for (auto& kv : owned_) keys.push_back(kv.first);
    owned_.clear(); q_.clear(); clockRunning_ = false; up = portUp_; }
  if (up) for (uint16_t k : keys) {
    uint8_t b[3] = {(uint8_t)(0x80 | (k >> 7)), (uint8_t)(k & 0x7F), 0};
    port_.send(b, 3);
  }
}

MidiOutStats MidiOutputScheduler::stats() const {
  std::lock_guard<std::mutex> l(m_);
  MidiOutStats s = st_; s.pending = q_.size(); s.ownedNotes = owned_.size(); return s;
}
MidiJitter MidiOutputScheduler::jitter() const {
  std::vector<float> v; MidiJitter j;
  { std::lock_guard<std::mutex> l(m_); v = jit_; j.maxErrNs = jitMax_; j.missedPulses = st_.clockSkipped; }
  j.count = v.size();
  if (v.empty()) return j;
  size_t mid = v.size() / 2, p95 = std::min(v.size() - 1, (size_t)(v.size() * 0.95));
  std::nth_element(v.begin(), v.begin() + mid, v.end()); j.medianErrNs = v[mid];
  std::nth_element(v.begin(), v.begin() + p95, v.end()); j.p95ErrNs = v[p95];
  return j;
}

} // namespace ss
