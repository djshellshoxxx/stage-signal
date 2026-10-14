// MOD-12 transport seam. HONEST SCOPE:
//   * No RTMP/RTMPS/SRT protocol stack exists in this build. TransportFactory returns Err::UnsupportedCapability with the
//     reason until a qualified media worker (e.g. FFmpeg built with libsrt + TLS, separate process, replaceable LGPL
//     build) registers a factory with registerMediaWorker(). Nothing here pretends to speak those protocols.
//   * Real and working: plain RTP/UDP sender (MJPEG RFC 2435 + L16 RFC 3551 + RTCP SR), see broadcast/rtp.h, behind IUdpSocket.
//   * LoopbackTransport / FaultyTransport are TEST DOUBLES (no sockets, no network).
// The stream secret is handed to ITransport::connect() inside TransportParams only (memory, never a URL / argv / log).
#pragma once
#include "broadcast/profile.h"
#include <functional>
#include <memory>
#include <deque>
#include <mutex>
namespace ss {

enum class PacketKind : uint8_t { Headers, Video, Audio };
enum class PacketCodec : uint8_t { Opaque, Mjpeg, H264, Aac, PcmF32 };
struct MediaPacket {
  PacketKind kind = PacketKind::Video; PacketCodec codec = PacketCodec::Opaque;
  Ns pts = 0; bool key = false;
  uint32_t frames = 0;                    // PcmF32: frames in `data` (interleaved float32)
  std::vector<uint8_t> data;
  uint64_t seq = 0;
};
using MediaPacketPtr = std::shared_ptr<const MediaPacket>;

enum class TransportErrorClass : uint8_t { Network, Receiver, Auth, Certificate, InvalidConfig };
inline bool isTerminal(TransportErrorClass c) { return c == TransportErrorClass::Auth || c == TransportErrorClass::Certificate || c == TransportErrorClass::InvalidConfig; }
const char* errorClassName(TransportErrorClass c);

struct TransportEvent {
  enum class Type : uint8_t { Connected, Disconnected } type = Type::Connected;
  TransportErrorClass cls = TransportErrorClass::Network;
  std::string detail;                     // library text; the session redacts it before it is stored or logged
};
struct TransportStats { uint64_t packetsSent = 0, bytesSent = 0, packetsRejected = 0; };

struct TransportParams {
  BroadcastSettings settings;
  std::string secret;                     // stream key / SRT passphrase, resolved from ISecretStore; NEVER log or put in argv/URL
  std::string passphrase;                 // SRT passphrase (resolved)
  Ns connectTimeoutNs = 15 * kNsPerSec;
};

struct ITransport {
  virtual ~ITransport() = default;
  // Starts an attempt; result is reported via cb (any thread, possibly synchronously). After close() no callback may fire.
  virtual Status connect(const TransportParams& p, std::function<void(const TransportEvent&)> cb) = 0;
  // Non-blocking. Err::ResourceExhausted = backpressure (caller keeps the packet), Err::TransportFailure = link broke.
  virtual Status send(const MediaPacket& pkt) = 0;
  virtual void close() = 0;               // must return promptly (< 500 ms); idempotent
  virtual TransportStats stats() const = 0;
  virtual std::string describe() const = 0;   // short label, no secrets
};

// ---- Test doubles ----------------------------------------------------------------------------------------
struct ConnectOutcome {                   // one scripted outcome per connect() attempt
  enum class Kind : uint8_t { Succeed, Fail, Hang } kind = Kind::Succeed;   // Hang = never answers (timeout path)
  TransportErrorClass cls = TransportErrorClass::Network; std::string detail = "connection refused";
};
class FaultyTransport : public ITransport {
 public:
  // Behaves like a loopback receiver (records packets) driven by a script. Not a network client.
  std::deque<ConnectOutcome> script;      // consumed per attempt; empty => Succeed
  int dropLinkAfterPackets = -1;          // after N delivered packets the "link" breaks (Disconnected, Network)
  size_t acceptLimit = (size_t)-1;        // backpressure: reject sends beyond this many accepted packets
  bool deferEvents = false;               // hold events until fire() (to test late/stale callbacks)
  std::string breakDetail = "connection reset by peer";
  Status connect(const TransportParams& p, std::function<void(const TransportEvent&)> cb) override;
  Status send(const MediaPacket& pkt) override;
  void close() override;
  TransportStats stats() const override { std::lock_guard<std::mutex> l(m); return st; }
  std::string describe() const override { return "faulty-test-transport"; }
  void fire();                            // deliver deferred events
  void breakLink(TransportErrorClass c = TransportErrorClass::Network, const std::string& d = "link down");
  std::vector<MediaPacket> received() const { std::lock_guard<std::mutex> l(m); return got; }
  std::string lastSecretSeen() const { std::lock_guard<std::mutex> l(m); return secretSeen; }   // test hook: proves the secret reached connect()
  int connectCalls() const { std::lock_guard<std::mutex> l(m); return connects; }
  bool closed() const { std::lock_guard<std::mutex> l(m); return isClosed; }
 private:
  void emit(const TransportEvent& e);
  mutable std::mutex m; TransportStats st; std::vector<MediaPacket> got; std::function<void(const TransportEvent&)> cb_; std::deque<TransportEvent> deferred;
  std::string secretSeen; int connects = 0; bool isClosed = false, connected = false; size_t accepted = 0;
};
using LoopbackTransport = FaultyTransport;   // an unscripted FaultyTransport is a loopback

struct IUdpSocket;
class TransportFactory {
 public:
  using MakeTransport = std::function<std::unique_ptr<ITransport>()>;
  using MakeSocket = std::function<std::unique_ptr<IUdpSocket>()>;
  // A qualified media worker adapter (FFmpeg w/ libsrt+TLS, separate process) registers per protocol.
  void registerMediaWorker(BroadcastProtocol p, MakeTransport f) { workers_[(int)p] = std::move(f); }
  // Windows UDP socket adapter (src/platform/windows) registers here; tests register FakeUdpSocket.
  void setUdpSocketFactory(MakeSocket f) { sock_ = std::move(f); }
  // Test hook: force a specific transport (e.g. FaultyTransport) for any protocol.
  void setTestOverride(MakeTransport f) { override_ = std::move(f); }
  Status create(const BroadcastSettings& s, std::unique_ptr<ITransport>& out) const;
  static std::string unsupportedReason(BroadcastProtocol p);
 private:
  MakeTransport workers_[4]; MakeSocket sock_; MakeTransport override_;
};

} // namespace ss
