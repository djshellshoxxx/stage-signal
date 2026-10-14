// MOD-12 BroadcastSession (IF-10 broadcast half; spec §15, REQ-B01..B07, REQ-P02).
//   Off -> Preparing -> Connecting -> Live -> Reconnecting | Stopping -> Off ; any failure -> Error.
//  * Explicit user start only: start() needs a UserStartToken that only UserActionGate::issue() can mint, and the gate
//    refuses ActionSource::Show / System / Remote. Show load, host restart and benchmark have no code path to a token and the
//    session has no persisted "was live" state, so none of them can start transmission (REQ-B02, REQ-P02).
//  * Secrets: settings hold a REFERENCE; the key is read from ISecretStore at each connect attempt, handed to the transport
//    in-memory only, and is scrubbed from every string this class stores, logs or exports. Never in a URL or argv.
//  * Reconnect (default on, this session only): 1/2/4/8/15 s (15 s thereafter) with +-20% jitter, 120 s deadline from the
//    start of the outage, fresh transport + headers + forced key frame; queued old media abandoned. Auth / certificate /
//    invalid-config failures are terminal. stop() is synchronous: it bumps the generation so late callbacks from any earlier
//    attempt are counted and ignored, and cancels reconnect immediately (well inside the 1 s requirement).
//  * Threading: start/stop/tick/status serialize on one mutex and call into the transport; transports must never call back
//    into the session except through the event callback (which only enqueues). submit() is wait-free for the producer
//    (bounded PacketQueue, drop-with-counter). Local recording is a separate object and is never touched.
//  * "Live" = connected and at least one media packet accepted by the transport after connect. It does not prove a public
//    service displays the stream, and for RTP (no feedback) not even that anyone receives it.
#pragma once
#include "broadcast/packet_queue.h"
#include "broadcast/transport.h"
#include "recording/video_encoder.h"
namespace ss {

enum class BroadcastState : uint8_t { Off, Preparing, Connecting, Live, Reconnecting, Stopping, Error };
const char* broadcastStateName(BroadcastState s);

class UserActionGate;
class UserStartToken {                          // move-only, single use, short-lived
 public:
  UserStartToken() = default;
  UserStartToken(const UserStartToken&) = delete; UserStartToken& operator=(const UserStartToken&) = delete;
  UserStartToken(UserStartToken&& o) noexcept : nonce_(o.nonce_), issued_(o.issued_), src_(o.src_) { o.nonce_ = 0; }
  UserStartToken& operator=(UserStartToken&& o) noexcept { nonce_ = o.nonce_; issued_ = o.issued_; src_ = o.src_; o.nonce_ = 0; return *this; }
  bool valid() const { return nonce_ != 0; }
  uint64_t nonce() const { return nonce_; } Ns issuedNs() const { return issued_; } ActionSource source() const { return src_; }
 private:
  friend class UserActionGate;
  UserStartToken(uint64_t n, Ns t, ActionSource s) : nonce_(n), issued_(t), src_(s) {}
  uint64_t nonce_ = 0; Ns issued_ = 0; ActionSource src_ = ActionSource::UI;
};
// Called by the UI layer when the operator presses Start (UI click, keyboard, or their own MIDI controller).
class UserActionGate {
 public:
  explicit UserActionGate(IClock* c) : clock_(c) {}
  Status issue(ActionSource src, UserStartToken& out);   // PermissionDenied for Show/System/Remote (cues, load, benchmark, restart)
  static bool sourceMayStart(ActionSource s) { return s == ActionSource::UI || s == ActionSource::Keyboard || s == ActionSource::Midi; }
 private:
  IClock* clock_; uint64_t next_ = 1;
};
constexpr Ns kTokenTtlNs = 30 * kNsPerSec;
constexpr Ns kReconnectDeadlineNs = 120 * kNsPerSec;
constexpr Ns kConnectTimeoutNs = 15 * kNsPerSec;
extern const int kReconnectDelaysSec[5];       // 1,2,4,8,15

struct BroadcastDeps {
  ISecretStore* secrets = nullptr; IClock* clock = nullptr; TransportFactory* factory = nullptr;
  const EncoderRegistry* encoders = nullptr;    // optional: verify the video encoder exists before connecting
  std::function<void(const std::string&)> log;  // receives already-redacted lines only
  std::function<void()> requestKeyframe;        // called after (re)connect so the encoder emits headers + an IDR/key frame
  uint32_t rngSeed = 1;                         // jitter source (deterministic in tests)
};

struct BroadcastStatus {
  BroadcastState state = BroadcastState::Off;
  bool connected = false; bool mediaFlowing = false; bool live = false;
  BroadcastProtocol protocol = BroadcastProtocol::RTMPS; std::string endpoint;   // redacted display URL, no key
  std::string encryption; bool encrypted = false;
  int attempt = 0; int64_t reconnectInMs = -1; int64_t outageMs = 0; uint64_t reconnects = 0;
  double bitrateKbps = 0; int targetKbps = 0;
  uint64_t packetsSent = 0, bytesSent = 0, packetsDropped = 0, queueDepth = 0, droppedWhileOffline = 0, lateCallbacksIgnored = 0;
  Status error; bool errorTerminal = false;
  std::vector<std::string> warnings; std::string text; uint64_t generation = 0;
};

class BroadcastSession {
 public:
  explicit BroadcastSession(const BroadcastDeps& d);
  ~BroadcastSession();
  BroadcastSession(const BroadcastSession&) = delete; BroadcastSession& operator=(const BroadcastSession&) = delete;

  // Shown to the operator BEFORE Start (what leaves the machine, security label). Never contains a secret.
  static std::string summary(const BroadcastSettings& s, const PublicationInputs& in) { return publicationSummary(s, in); }

  Status start(UserStartToken&& token, const BroadcastSettings& s, const PublicationInputs& pub = PublicationInputs());
  void stop();                                  // idempotent; Off immediately; also acknowledges Error
  void tick();                                  // drive: events, timers, packet pump (call every ~20-50 ms from the worker)
  bool submit(const MediaPacketPtr& p);         // producer side; never blocks
  void setHeaders(const MediaPacketPtr& h);     // codec config re-sent first after every (re)connect
  BroadcastStatus status() const;
  BroadcastState state() const;
  void setStateCallback(std::function<void(BroadcastState)> cb);   // invoked after internal locks are released
 struct Impl;                                 // opaque implementation (public only so the .cpp helpers can name it)
 private:
  std::unique_ptr<Impl> p_;
};

} // namespace ss
