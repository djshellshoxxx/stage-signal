// MOD-10 LightingEngine: fixture resolution, blackout, dry-run, transport (spec §14, REQ-L01..L06, REQ-S12, IF-08).
//
// SAFETY / HONESTY NOTES (also in docs/LIGHTING.md):
//  * The engine CANNOT guarantee physical blackout after a cable/network/power loss. Fixtures and nodes may hold
//    the last received DMX values. Configure node/fixture DMX-loss behaviour (Art-Net node hold/timeout, sACN receiver
//    network-data-loss timeout, fixture "no DMX" mode) and test it before relying on it.
//  * Default state is Disarmed. Nothing is transmitted while Disarmed. Any fault/disconnect/reconnect/patch change leaves
//    the engine Disarmed; it NEVER re-arms by itself. Arming needs an explicit one-shot ArmToken (REQ-L05).
//  * Dry-run engines (makeDryRun) own a DryRunSink internally and have NO way to receive a real sink (REQ-L03).
//  * Precedence per channel: emergency blackout > lighting blackout > Disable-Strobing > manual override > programmed cue >
//    audio modulation > profile default. Blackout is fixture-aware (profile blackout/safe values, not blind zeros).
//  * Audio mappings read the latest FeatureSnapshot at lighting tick rate only. Fades run in the physical (raw DMX) domain.
//  * Blackout/Disable-Strobing flags are lock-free atomics read at the start of each tick: effective by the NEXT tick.
#pragma once
#include "core/types.h"
#include "core/ports.h"
#include "core/queue.h"
#include "lighting/artnet.h"
#include "lighting/sacn.h"
#include "lighting/fixture.h"
#include "lighting/usb_dmx.h"
#include "lighting/mock_sink.h"
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace ss {

enum class LightProtocol : uint8_t { ArtNet, Sacn, UsbDmx };
constexpr double kDefaultTickHz = 25.0, kMaxTickHz = 40.0;

struct EngineConfig {
  LightProtocol protocol = LightProtocol::ArtNet;
  std::string host;                 // unicast destination (ignored for sACN multicast)
  bool sacnMulticast = false;       // sACN: 239.255.hi.lo instead of unicast
  std::string nicAddr;              // selected interface ("" = default)
  bool allowBroadcast = false;      // explicit opt-in; broadcast destinations rejected otherwise
  double tickHz = kDefaultTickHz;   // 1..40
  int slotsPerFrame = 512;          // even, 2..512
  std::string sourceName = "StageSignal";
  uint8_t sacnPriority = sacn::kDefaultPriority;
  sacn::Cid cid{};                  // sACN source CID (all-zero => derived from sourceName)
  uint8_t artnetPhysical = 0;
  Ns audioStaleNs = kNsPerSec;      // audio layer inactive when newest snapshot is older than this
};

enum class ArmState : uint8_t { Disarmed, Armed };
struct ArmToken { uint64_t epoch = 0, nonce = 0; bool valid() const { return nonce != 0; } };

struct CueValue { std::string fixtureId, channel; uint32_t value = 0; };
struct Cue { std::string name; std::vector<CueValue> values; Ns fadeNs = 0; };
enum class AudioSource : uint8_t { RmsDb, BandLow, BandMid, BandHigh, Onset };
struct AudioMapping { std::string fixtureId, channel; AudioSource source = AudioSource::BandLow; float inLo = 0, inHi = 1; uint32_t outLo = 0, outHi = 255; };

struct UniverseFrame { int universe = 0; uint16_t length = 0; uint64_t tick = 0; uint8_t data[512] = {}; };

struct LightEvent {
  enum Kind : uint8_t { Armed, Disarmed, Fault, Reconnected, Warning, Blackout, Strobe } kind = Warning;
  Ns t = 0; std::string detail;
};

struct EngineStatus {
  ArmState state = ArmState::Disarmed;
  bool dryRun = false, transportOpen = false, faulted = false;
  bool lightingBlackout = false, emergencyBlackout = false, strobeDisabled = false;
  size_t unresolvedConflicts = 0, fixtures = 0;
  uint64_t ticks = 0, skippedTicks = 0, framesSent = 0, faults = 0;
  Status lastFault;
};

class LightingEngine {
 public:
  static Status validateConfig(const EngineConfig&, bool dryRun);
  // Live engines transmit through the given sink/device. Return nullptr (and fill *err) on invalid config.
  static std::unique_ptr<LightingEngine> makeLive(IClock&, const EngineConfig&, IDatagramSink& sink, Status* err = nullptr);
  static std::unique_ptr<LightingEngine> makeLiveUsb(IClock&, const EngineConfig&, IUsbDmxDevice& dev, Status* err = nullptr);
  // Preview/setup/benchmark: owns its own DryRunSink. Cannot be given a real sink.
  static std::unique_ptr<LightingEngine> makeDryRun(IClock&, const EngineConfig&, Status* err = nullptr);
  ~LightingEngine();   // graceful stop; destroy the engine BEFORE the sink/device it uses
  LightingEngine(const LightingEngine&) = delete; LightingEngine& operator=(const LightingEngine&) = delete;

  // ---- patch / content
  Status applyPatch(const Patch&, const ProfileLibrary&, PatchReport* report = nullptr);   // forces Disarmed
  Status setManual(const std::string& fixtureId, const std::string& channel, uint32_t raw);
  void clearManual(const std::string& fixtureId, const std::string& channel);
  void clearAllManual();
  Status setCue(const Cue&);                          // fades from current cue-layer values
  void clearCue();
  Status setAudioMappings(const std::vector<AudioMapping>&);
  void submitFeatures(const FeatureSnapshot&);        // latest-wins mailbox, consumed at tick rate

  // ---- safety controls (lock-free latches; effective by next tick)
  void setLightingBlackout(bool on);
  void setEmergencyBlackout(bool on);
  void setStrobeDisabled(bool on);
  ActionResult handleAction(const Action&);           // LightingBlackout/EmergencyBlackout/RestoreBlackout/DisableStrobing/Arm/Disarm

  // ---- arming (REQ-L05)
  Status requestArmToken(ArmToken& out);              // operator-confirmed, single use, bound to current epoch
  Status arm(const ArmToken&);
  void disarm();                                      // graceful: safe frames (+ sACN termination), then silent

  // ---- transport
  Status openTransport();
  Status reconnect();                                 // reopen after fault; STAYS Disarmed
  void notifyTransportLost(const std::string& detail);// platform reports link loss: fault + Disarmed

  // ---- scheduling
  int pump();                                         // run all ticks due on the injected clock; returns ticks run
  Status start();                                     // opens transport if needed, starts worker thread
  void stop();                                        // graceful: stops thread, safe frames + sACN termination, closes transport

  // ---- observation
  EngineStatus status() const;
  std::vector<LightEvent> drainEvents();
  bool latestFrame(int universe, UniverseFrame& out) const;
  std::vector<int> universes() const;
  const DryRunSink* dryRunSink() const { return dry_.get(); }
  bool resolvedValue(const std::string& fixtureId, const std::string& channel, uint32_t& out) const;  // last resolved tick

 private:
  struct Fix { PatchEntry e; FixtureProfile p; int uidx = 0; };
  LightingEngine(IClock&, const EngineConfig&, IDatagramSink*, IUsbDmxDevice*, std::unique_ptr<DryRunSink>);
  bool findLocked(const std::string& fid, const std::string& ch, int& f, int& c) const;
  void tickLocked(Ns now);
  void resolveLocked(bool blackout, bool noStrobe, std::vector<UniverseFrame>& out);
  void updateCueLocked(Ns now);
  void audioLocked(Ns now);
  Status sendFrameLocked(size_t uidx, const uint8_t* data, bool terminate);
  void transmitLocked();
  void faultLocked(const Status&);
  void disarmLocked(const char* why, bool graceful);
  void sendSafeAndTerminateLocked();
  Status openLocked();
  void eventLocked(LightEvent::Kind, const std::string&);

  IClock& clock_; EngineConfig cfg_;
  IDatagramSink* sink_ = nullptr; IUsbDmxDevice* usb_ = nullptr; std::unique_ptr<DryRunSink> dry_;
  bool dryRun_ = false;
  mutable std::mutex m_;
  ArmState state_ = ArmState::Disarmed;
  bool transportOpen_ = false, faulted_ = false, started_ = false;
  uint64_t epoch_ = 1, nonceCtr_ = 0, issuedNonce_ = 0;
  std::atomic<uint64_t> revision_{0};
  std::atomic<bool> emergency_{false}, lblackout_{false}, nostrobe_{false};
  std::vector<Fix> fix_; std::vector<int> order_;      // order_: write order by ownership rank
  std::vector<int> universes_;
  std::vector<std::unique_ptr<Mailbox<UniverseFrame>>> boxes_;
  std::vector<UniverseFrame> work_, lastResolved_;
  std::vector<uint8_t> artSeq_, sacnSeq_; std::vector<std::string> mcast_;
  size_t unresolved_ = 0; bool patchOk_ = false;
  // layers: [fixture][channel], -1 = unset
  std::vector<std::vector<int64_t>> manual_, cueFrom_, cueTo_, cueCur_, audio_;
  bool fading_ = false; Ns fadeStart_ = 0, fadeDur_ = 0;
  struct MapIdx { int f, c; AudioMapping m; };
  std::vector<MapIdx> maps_;
  Mailbox<FeatureSnapshot> features_; std::atomic<Ns> featuresAt_{-1};
  bool prevEmer_ = false, prevLb_ = false, prevNs_ = false;
  Ns nextTick_ = 0, period_ = 0;
  uint64_t ticks_ = 0, skipped_ = 0, framesSent_ = 0, faults_ = 0; Status lastFault_;
  std::deque<LightEvent> events_;
  std::array<uint8_t, 640> pkt_{};
  sacn::SourceInfo src_;
  std::thread th_; std::atomic<bool> stopFlag_{false}; bool running_ = false;
};

} // namespace ss
