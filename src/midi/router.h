// MOD-09 MIDI input router + route graph + feedback protection (REQ-M03/M04, spec §8, §14). Portable C++17.
#pragma once
#include "core/types.h"
#include "core/ports.h"
#include "midi/mapping.h"
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ss {

// ---- origin tagging: Action.correlation carries a MIDI origin tag so MIDI-derived actions can never
// be re-emitted as MIDI (action -> MIDI -> action recursion is forbidden).
constexpr uint64_t kMidiOriginTag = 0x4D49;   // 'MI'
inline uint64_t makeMidiCorrelation(uint8_t port, uint64_t seq) { return (kMidiOriginTag << 48) | ((uint64_t)port << 40) | (seq & 0xFFFFFFFFFFULL); }
inline bool isMidiOriginated(const Action& a) { return a.source == ActionSource::Midi || (a.correlation >> 48) == kMidiOriginTag; }
inline bool actionMayEmitMidi(const Action& a) { return !isMidiOriginated(a); }

// Fixed 1 s window ceiling (default 1,000 msgs/s/port). Not thread-safe; caller serializes.
class RateLimiter {
 public:
  explicit RateLimiter(int perSec = 1000) : max_(perSec) {}
  bool allow(Ns now) {
    if (win_ == kNone || now - win_ >= kNsPerSec || now < win_) { win_ = now; n_ = 0; }
    if (n_ >= max_) { ++dropped_; return false; }
    ++n_; return true;
  }
  void setCeiling(int perSec) { max_ = perSec; }
  uint64_t dropped() const { return dropped_; }
  void reset() { win_ = kNone; n_ = 0; }
 private:
  static constexpr Ns kNone = INT64_MIN;
  int max_; Ns win_ = kNone; int n_ = 0; uint64_t dropped_ = 0;
};

// ---- route graph ---------------------------------------------------------------------------
enum class RouteKind : uint8_t { InputToAction, ActionToOutput, InputToOutput /*thru*/ };
struct MidiRoute {
  std::string id;
  RouteKind kind = RouteKind::InputToAction;
  std::string inPort, outPort;          // port stable ids (unused side empty)
  bool enabled = false;
  bool explicitlyEnabled = false;       // operator confirmed a feedback-risk route
  bool suspended = false;               // loop protection tripped
  std::string suspendReason;
};
// Feedback-risk: thru routes, and action->output routes whose output port belongs to the same physical
// device (group) as any input route (device echo can re-enter as input). Risky routes start disabled.
class MidiRouteGraph {
 public:
  void declareSameDevice(const std::string& portA, const std::string& portB);   // groups default to stableId
  Status addRoute(MidiRoute r);          // id required unique; non-risky routes enabled, risky disabled
  bool removeRoute(const std::string& id);
  Status enable(const std::string& id, bool explicitConfirm);   // risky routes need explicitConfirm
  void disable(const std::string& id);
  bool suspend(const std::string& id, const std::string& reason);
  bool isActive(const std::string& id) const;   // enabled && !suspended
  bool feedbackRisk(const std::string& id) const;
  bool get(const std::string& id, MidiRoute& out) const;
  std::vector<MidiRoute> list() const;
 private:
  std::string group(const std::string& port) const;
  bool riskLocked(const MidiRoute& r) const;
  void revalidateLocked();
  mutable std::mutex m_;
  std::vector<MidiRoute> r_;
  std::vector<std::pair<std::string, std::string>> same_;
};

// ---- router --------------------------------------------------------------------------------
enum class MidiAlarmKind : uint8_t { PortLost, PortReconnected, InputOverflow, RateCeiling, EchoLoop };
struct MidiAlarm { MidiAlarmKind kind; int port = -1; std::string portId; uint64_t count = 0; std::string detail; };

struct RouterStats {
  uint64_t received = 0, realtime = 0, ignored = 0, unmapped = 0, learned = 0;
  uint64_t queued = 0, coalescedAtIngest = 0, droppedStaleCc = 0, droppedOldest = 0, loss = 0;
  uint64_t rateDropped = 0, suspendedDropped = 0, staleGeneration = 0, latched = 0;
  uint64_t debounced = 0, coalescedAtPump = 0, emitted = 0, echoTrips = 0;
  size_t queueDepth = 0, queueCapacity = 512;
};

class MidiRouter {
 public:
  using ActionSink = std::function<void(const Action&)>;
  static constexpr size_t kQueueCap = 512;
  static constexpr size_t kLatchCap = 128;   // distinct critical controls pending
  static constexpr Ns kDebounceNs = 50 * kNsPerMs;
  MidiRouter(MappingTable& table, MidiRouteGraph& graph, IClock& clock);
  ~MidiRouter();                          // detaches attached ports (closeInput + clears lost callback)

  void setActionSink(ActionSink s);
  void setAlarmCallback(std::function<void(const MidiAlarm&)> cb);   // delivered from pump(), never from MIDI callback
  void setRateCeiling(int perSecPerPort);                            // default 1000
  void setEchoThreshold(int identicalPerSec);                        // default 20 (trips when exceeded within 1 s)

  // Registers an input port (creates its InputToAction route "in:<stableId>"). Returns port index.
  int addPort(const std::string& stableId, const std::string& name);
  // addPort + openInput + lost callback wiring. `lostForward` also sees output losses.
  Status attach(IMidiPort& port, const MidiPortInfo& info, int* portIdx = nullptr);
  void setLostForward(std::function<void(int portIdx, bool input)> f);
  Status reconnect(int portIdx);          // reopen after loss: new generation, no stale actions survive
  void onPortLost(int portIdx);           // from IMidiPort lost callback; mapping unavailable immediately
  void onPortReconnected(int portIdx);    // marks available again (called by reconnect())
  void checkPorts();                      // fallback poll: inputAlive()==false => onPortLost

  // Producer (MIDI callback thread): copy-only, bounded, non-blocking apart from a short mutex.
  void onBytes(int portIdx, Ns arrivalNs, const uint8_t* d, size_t n);
  // Consumer thread: drain latch first, then queue; emits Actions to the sink. Returns actions emitted.
  size_t pump();
  size_t pumpCritical();                  // latch only

  bool alarmLatched() const;
  void clearAlarm();                      // does not re-enable suspended routes (operator must enable explicitly)
  RouterStats stats() const;
  uint32_t portGeneration(int portIdx) const;
  std::string routeIdForPort(int portIdx) const;

 private:
  struct Sig { uint32_t key = 0; Ns ts[24]; int n = 0, head = 0; Ns last = 0; bool used = false; };
  struct Port {
    std::string id, name, route; IMidiPort* port = nullptr;
    uint32_t gen = 1; bool alive = true; RateLimiter rate; Sig sigs[8];
  };
  struct QEv { int port; uint32_t gen; MidiMsg msg; bool continuous; bool critical; bool relative; };
  struct Debounce { uint32_t key; Ns last; };
  bool echoCheckLocked(Port& p, const MidiMsg& m, Ns now);
  void raiseLocked(MidiAlarmKind k, int port, uint64_t count, const std::string& detail);
  size_t deliver(std::vector<QEv>& evs, bool ordinary);

  MappingTable& table_; MidiRouteGraph& graph_; IClock& clock_;
  mutable std::mutex m_;
  std::vector<Port> ports_;
  std::vector<QEv> q_, latch_;
  std::vector<MidiAlarm> alarms_;
  std::vector<Debounce> deb_;
  ActionSink sink_; std::function<void(const MidiAlarm&)> alarmCb_; std::function<void(int, bool)> lostFwd_;
  RouterStats st_; bool latched_ = false; int rateCeiling_ = 1000; int echoMax_ = 20; uint64_t seq_ = 0;
  uint64_t lossReported_ = 0;
};

} // namespace ss
