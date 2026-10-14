// MOD-09 MIDI output scheduler (REQ-M02, IF-08, spec §14): monotonic deadlines, notes/CC, 24 ppqn clock,
// owned-note tracking, SysEx gate, jitter statistics. Portable C++17.
#pragma once
#include "core/types.h"
#include "core/ports.h"
#include "midi/router.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace ss {

enum class MidiOrigin : uint8_t { Operator, Show, System, MidiInput /*forbidden: action->MIDI->action recursion*/ };

struct MidiOutMessage {              // IF-08 ScheduledOutput (MIDI form)
  std::vector<uint8_t> bytes;
  Ns deadline = 0;                   // monotonic ns; 0 or past => as soon as possible
  MidiOrigin origin = MidiOrigin::Operator;
  uint32_t generation = 0;           // 0 = current; otherwise must equal generation() or it is discarded as stale
  uint64_t armToken = 0;             // 0 = unarmed message; otherwise must equal the current arm token
};

struct MidiOutConfig {
  bool sysexEnabled = false;         // default off
  size_t maxSysexBytes = 1024;       // including F0/F7
  size_t queueCap = 512;
  Ns staleNs = 20 * kNsPerMs;        // deadlines older than this are coalesced (CC) before sending
  int maxMsgsPerSec = 1000;          // per port; realtime and note-off are exempt
  bool allowStart = false, allowStop = false, allowContinue = false;   // each mapped/enabled separately
};

struct MidiJitter {                  // clock interval error |actual interval - nominal period|
  size_t count = 0;
  double medianErrNs = 0, p95ErrNs = 0, maxErrNs = 0;
  uint64_t missedPulses = 0;         // pulses skipped because the worker stalled > 1 period
};
struct MidiOutStats {
  uint64_t sent = 0, notes = 0, cc = 0, sysex = 0, transport = 0, clockPulses = 0, clockSkipped = 0;
  uint64_t rejected = 0, queueFull = 0, coalesced = 0, rateDropped = 0, sendFailures = 0, staleGeneration = 0;
  size_t pending = 0, ownedNotes = 0;
};

class MidiOutputScheduler {
 public:
  MidiOutputScheduler(IClock& clock, IMidiPort& port, MidiOutConfig cfg = MidiOutConfig());
  ~MidiOutputScheduler();
  MidiOutputScheduler(const MidiOutputScheduler&) = delete;
  MidiOutputScheduler& operator=(const MidiOutputScheduler&) = delete;

  void setConfig(const MidiOutConfig& c);
  MidiOutConfig config() const;

  // Validates then enqueues. Never blocks on the port. Err codes: InvalidArgument, PermissionDenied (SysEx off,
  // transport off, forbidden origin), ResourceExhausted (queue full), DeviceLost (port down), Cancelled (stale).
  Status submit(MidiOutMessage m);
  Status noteOn(int ch, int note, int vel, Ns deadline = 0, MidiOrigin o = MidiOrigin::Operator);
  Status noteOff(int ch, int note, Ns deadline = 0, MidiOrigin o = MidiOrigin::Operator);
  Status cc(int ch, int ctrl, int value, Ns deadline = 0, MidiOrigin o = MidiOrigin::Operator);
  Status programChange(int ch, int program, Ns deadline = 0, MidiOrigin o = MidiOrigin::Operator);
  Status sysex(const std::vector<uint8_t>& bytes, Ns deadline = 0, MidiOrigin o = MidiOrigin::Operator);
  Status sendStart(Ns deadline = 0);
  Status sendStop(Ns deadline = 0);
  Status sendContinue(Ns deadline = 0);

  // Clock (opt-in, 24 pulses per quarter note). Tempo source is polled each pulse; <=0 / NaN pauses pulses.
  void setTempoSource(std::function<double()> bpm);
  void setFixedTempo(double bpm);
  void setClockEnabled(bool on);
  bool clockEnabled() const;

  // Owned notes: only notes StageSignal sent are released; no All Notes Off is ever sent.
  size_t ownedNoteCount() const;
  size_t releaseOwnedNotes();        // enqueues note-offs (priority); returns count

  // Gates for IF-08.
  uint32_t generation() const;
  void arm(uint64_t token);          // messages carrying a different non-zero token are discarded
  void disarm();
  void notePortLost();               // clears queue + owned set, pauses clock, bumps generation
  void notePortRestored();           // resumes (no stale queued messages survive)

  // Deterministic stepping (tests/FakeClock): sends everything due at clock.nowNs(); returns next deadline or INT64_MAX.
  Ns step();
  static constexpr Ns kNoDeadline = INT64_MAX;

  // Worker thread. stop() is idempotent, safe before start()/from destructor, joins, then releases owned notes.
  void start();
  void stop();
  bool running() const;

  MidiOutStats stats() const;
  MidiJitter jitter() const;

 private:
  struct Pending { Ns deadline; uint64_t seq; MidiOutMessage msg; bool priority; };
  struct Due { Ns deadline; MidiOutMessage msg; bool pulse = false; Ns period = 0; bool gapBefore = false; };
  Status validate(const MidiOutMessage& m) const;
  Status enqueueLocked(MidiOutMessage&& m, bool priority);
  void run();

  IClock& clock_; IMidiPort& port_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  MidiOutConfig cfg_;
  std::vector<Pending> q_;
  uint64_t seq_ = 0; uint32_t gen_ = 1; uint64_t arm_ = 0; bool portUp_ = true;
  std::map<uint16_t, int> owned_;                  // (ch<<7|note) -> count
  RateLimiter rate_;
  // clock
  bool clockOn_ = false, clockRunning_ = false, lastPulseValid_ = false, gap_ = false;
  Ns nextPulse_ = 0, lastPulseSend_ = 0;
  std::function<double()> tempo_; double fixedBpm_ = 0;
  // stats
  MidiOutStats st_; std::vector<float> jit_; size_t jitHead_ = 0; double jitMax_ = 0;
  // worker
  std::mutex life_; std::thread th_; bool stopFlag_ = false; std::atomic<bool> running_{false};
};

} // namespace ss
