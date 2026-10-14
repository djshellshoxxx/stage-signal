// MOD-12 RTMP / RTMPS publish client (spec §15; Adobe RTMP 1.0 + AMF0). Portable: all I/O goes through IByteStream.
// HONEST SCOPE: implemented from the public specification and verified ONLY against an in-process mock server
// (tests/test_cast_rtmp.cpp). It has NOT been qualified against any live receiver (nginx-rtmp, YouTube, Twitch, ...).
//  * Simple (unsigned) handshake only; no encrypted RTMPE, no complex/digest handshake, no AMF3 objects.
//  * Publish flow: C0/C1/C2, SetChunkSize, connect, releaseStream, FCPublish, createStream, publish(key,'live'),
//    onMetaData, then FLV-style video (AVC) / audio (AAC) messages. Server SetChunkSize / WindowAckSize / SetPeerBandwidth /
//    Ping handled. Chunk types 0-3 are produced and consumed, extended timestamps supported.
//  * Never blocks the caller: send() copies into a bounded queue; one worker thread owns the stream. When the queue is full the
//    OLDEST NON-KEYFRAME packet is dropped (counted), headers and video key frames are kept.
//  * The stream key is used only inside connect()'s worker (releaseStream / FCPublish / publish); every string handed to the event
//    callback is scrubbed of it. TLS: the IByteStream must validate chain AND host name (supportsTlsVerification()); an
//    RTMPS attempt over a stream that cannot is refused, never downgraded and never connected unverified.
//  * Failure classes: certificate / authentication-rejected / invalid app -> terminal (see isTerminal); everything else retryable.
// Media packet conventions (MediaPacket):
//   Video/H264: Annex B access unit (SPS/PPS/AUD are stripped; they travel in the headers), key = IDR. pts in ns.
//   Audio/Aac : one raw AAC-LC access unit (no ADTS; an ADTS header is stripped if present).
//   Headers   : codec H264 -> data = avcC; codec Aac -> data = AudioSpecificConfig; codec Opaque -> packHeaders() bundle.
#pragma once
#include "broadcast/transport.h"
namespace ss {

// Blocking byte pipe with timeouts. close() may be called from any thread at any time and must make connect/send/recv return
// promptly (< 1 s). All other calls come from the transport's single worker thread.
struct IByteStream {
  virtual ~IByteStream() = default;
  // Resolve + connect (+ TLS handshake when tls). timeout bounds the whole call. With tls the implementation MUST validate the
  // server certificate chain and that the host name matches `host`, else fail (see lastFailureWasCertificate()).
  virtual Status connect(const std::string& host, uint16_t port, bool tls, Ns timeout) = 0;
  // True only if connect(tls=true) validates chain + host name. False => RTMPS must be refused.
  virtual bool supportsTlsVerification() const = 0;
  // Returns bytes written (>0), 0 on timeout (nothing written), <0 on error/closed.
  virtual int send(const uint8_t* d, size_t n, int timeoutMs) = 0;
  // Returns bytes read (>0), 0 on timeout, <0 on error / orderly shutdown by the peer.
  virtual int recv(uint8_t* buf, size_t cap, int timeoutMs) = 0;
  virtual void close() = 0;                                    // idempotent, thread-safe, aborts blocked calls
  virtual std::string lastError() const { return std::string(); }          // text for the last failure (no secrets)
  virtual bool lastFailureWasCertificate() const { return false; }         // last connect failure was certificate/host validation
};
using MakeByteStream = std::function<std::unique_ptr<IByteStream>()>;

namespace rtmp {
using Bytes = std::vector<uint8_t>;

// ---------------- AMF0 ----------------
struct Value;
struct Prop;
struct Value {
  enum class T : uint8_t { Number, Bool, String, Object, Null, Undefined, EcmaArray, StrictArray } t = T::Null;
  double num = 0; bool b = false; std::string s; std::vector<Prop> props; std::vector<Value> items;
  static Value Num(double d) { Value v; v.t = T::Number; v.num = d; return v; }
  static Value Bool(bool x) { Value v; v.t = T::Bool; v.b = x; return v; }
  static Value Str(const std::string& x) { Value v; v.t = T::String; v.s = x; return v; }
  static Value Null() { return Value(); }
  static Value Obj() { Value v; v.t = T::Object; return v; }
  static Value Ecma() { Value v; v.t = T::EcmaArray; return v; }
  const Value* get(const std::string& key) const;              // object / ecma array member or nullptr
  std::string str(const std::string& key, const std::string& dflt = std::string()) const;
  Value& set(const std::string& key, const Value& v);          // append property
};
struct Prop { std::string key; Value val; };
void amfEncode(const Value& v, Bytes& out);
// Decodes one value at d[pos]; advances pos. Depth/size bounded; false on malformed or unsupported (AMF3 ref, movieclip, ...).
bool amfDecode(const uint8_t* d, size_t n, size_t& pos, Value& out, int depth = 0);

// ---------------- chunk stream ----------------
enum MsgType : uint8_t { kSetChunkSize = 1, kAbort = 2, kAck = 3, kUserControl = 4, kWindowAck = 5, kPeerBandwidth = 6,
                         kAudio = 8, kVideo = 9, kDataAmf3 = 15, kCmdAmf3 = 17, kData = 18, kCmd = 20 };
struct Message { uint32_t csid = 0, ts = 0, sid = 0; uint8_t type = 0; Bytes payload; };
constexpr size_t kDefaultChunkSize = 128, kMaxChunkSize = 1u << 20, kMaxIncomingMessage = 2u << 20;

class ChunkWriter {
 public:
  void setChunkSize(size_t n) { chunk_ = n < 1 ? 1 : (n > kMaxChunkSize ? kMaxChunkSize : n); }
  size_t chunkSize() const { return chunk_; }
  // Appends the chunked encoding of one message. Uses fmt 0 on the first message of a csid (or when the stream id changes or the
  // timestamp goes backwards), fmt 1/2/3 when the header can be compressed, fmt 3 for continuation chunks, extended timestamps >= 0xFFFFFF.
  void write(uint32_t csid, uint8_t type, uint32_t sid, uint32_t ts, const uint8_t* payload, size_t n, Bytes& out);
  struct Stats { uint64_t fmt[4] = {0, 0, 0, 0}; } stats;
 private:
  struct Cs { bool valid = false; uint32_t ts = 0, delta = 0, len = 0, sid = 0; uint8_t type = 0; };
  size_t chunk_ = kDefaultChunkSize; std::vector<Cs> cs_;
};
class ChunkReader {
 public:
  void setChunkSize(size_t n) { chunk_ = n < 1 ? 1 : (n > kMaxChunkSize ? kMaxChunkSize : n); }
  size_t chunkSize() const { return chunk_; }
  void abort(uint32_t csid);
  // Feeds bytes; every completed message is passed to `on` (which may call setChunkSize / abort). Returns false on a protocol
  // violation (oversized message, header continuation with no previous header, ...); `error` explains.
  bool feed(const uint8_t* d, size_t n, const std::function<void(Message&)>& on);
  std::string error;
 private:
  struct Cs { bool have = false, ext = false; uint32_t ts = 0, delta = 0, len = 0, sid = 0; uint8_t type = 0; Bytes buf; };
  size_t chunk_ = kDefaultChunkSize; Bytes in_; size_t pos_ = 0; std::vector<std::pair<uint32_t, Cs>> cs_;
  Cs& cs(uint32_t id);
};

// ---------------- FLV-style payloads ----------------
Bytes flvAvcSequenceHeader(const Bytes& avcC);                         // video payload: key frame, AVCPacketType 0
Bytes flvAvcNalu(bool key, const Bytes& avccSample, int32_t cts = 0);  // video payload: AVCPacketType 1
Bytes flvAacSequenceHeader(const Bytes& asc);                          // audio payload: AACPacketType 0
Bytes flvAacRaw(const uint8_t* aac, size_t n);                         // audio payload: AACPacketType 1
Bytes packHeaders(const Bytes& avcC, const Bytes& asc);                // bundle for MediaPacket{Headers, Opaque}
bool unpackHeaders(const uint8_t* d, size_t n, Bytes& avcC, Bytes& asc);
}  // namespace rtmp

struct RtmpConfig {
  size_t chunkSize = 4096;                 // our outgoing chunk size (announced with Set Chunk Size)
  size_t maxQueuePackets = 256, maxQueueBytes = 4u << 20;
  int pollMs = 10;                         // idle recv timeout (latency floor for stop / incoming messages)
  Ns sendStallNs = 10 * kNsPerSec;         // no write progress for this long => link considered dead
  Ns stepTimeoutNs = 0;                    // 0 => use TransportParams::connectTimeoutNs for the whole connect+publish phase
  std::string flashVer = "FMLE/3.0 (compatible; StageSignal)";
};
struct RtmpCounters {
  uint64_t droppedPackets = 0, droppedKeyframeBlocked = 0, messagesSent = 0, acksSent = 0, pingsAnswered = 0, serverChunkSizeChanges = 0;
  uint64_t queueHighWater = 0, bytesWire = 0, bytesReceived = 0; bool s2Matched = true;
};
class RtmpTransport : public ITransport {
 public:
  // `tls` selects RTMPS. The transport owns the stream. RTMPS over a stream without verification support fails connect() with
  // a Certificate-class event (terminal) before any socket is opened.
  RtmpTransport(std::unique_ptr<IByteStream> stream, bool tls, const RtmpConfig& cfg = RtmpConfig());
  ~RtmpTransport() override;
  Status connect(const TransportParams& p, std::function<void(const TransportEvent&)> cb) override;
  Status send(const MediaPacket& pkt) override;          // non-blocking; drops oldest non-key packet when full
  void close() override;                                 // joins the worker; no callback fires after it returns. Do not call from the callback.
  TransportStats stats() const override;
  std::string describe() const override;
  RtmpCounters counters() const;
 private:
  struct Impl; std::unique_ptr<Impl> p_;
};
std::unique_ptr<ITransport> makeRtmpTransport(std::unique_ptr<IByteStream> stream, bool tls, const RtmpConfig& cfg = RtmpConfig());

} // namespace ss
