#include "broadcast/transport.h"
#include "broadcast/rtp.h"
#include "broadcast/rtmp.h"
#include "broadcast/redact.h"
namespace ss {

const char* errorClassName(TransportErrorClass c) {
  switch (c) { case TransportErrorClass::Network: return "network"; case TransportErrorClass::Receiver: return "receiver"; case TransportErrorClass::Auth: return "authentication"; case TransportErrorClass::Certificate: return "certificate"; case TransportErrorClass::InvalidConfig: return "invalid-config"; }
  return "?";
}

// ---------------- FaultyTransport (test double) ----------------
void FaultyTransport::emit(const TransportEvent& e) {
  std::function<void(const TransportEvent&)> cb; bool defer;
  { std::lock_guard<std::mutex> l(m); cb = cb_; defer = deferEvents; if (defer) deferred.push_back(e); }
  if (!defer && cb) cb(e);
}
Status FaultyTransport::connect(const TransportParams& p, std::function<void(const TransportEvent&)> cb) {
  ConnectOutcome o;
  { std::lock_guard<std::mutex> l(m);
    ++connects; cb_ = std::move(cb); isClosed = false; connected = false; secretSeen = p.secret; accepted = 0;
    if (!script.empty()) { o = script.front(); script.pop_front(); } }
  if (o.kind == ConnectOutcome::Kind::Hang) return Status::Ok();
  TransportEvent e;
  if (o.kind == ConnectOutcome::Kind::Succeed) { e.type = TransportEvent::Type::Connected; std::lock_guard<std::mutex> l(m); connected = true; }
  else { e.type = TransportEvent::Type::Disconnected; e.cls = o.cls; e.detail = o.detail; }
  emit(e); return Status::Ok();
}
Status FaultyTransport::send(const MediaPacket& pk) {
  { std::lock_guard<std::mutex> l(m);
    if (isClosed || !connected) return Status::Fail(Err::TransportFailure, "not connected");
    if (accepted >= acceptLimit) { ++st.packetsRejected; return Status::Fail(Err::ResourceExhausted, "backpressure"); }
    got.push_back(pk); ++accepted; ++st.packetsSent; st.bytesSent += pk.data.size();
    if (dropLinkAfterPackets >= 0 && (int)st.packetsSent >= dropLinkAfterPackets) { connected = false; dropLinkAfterPackets = -1; }
    else return Status::Ok(); }
  TransportEvent e; e.type = TransportEvent::Type::Disconnected; e.cls = TransportErrorClass::Network; e.detail = breakDetail; emit(e);
  return Status::Ok();
}
void FaultyTransport::close() { std::lock_guard<std::mutex> l(m); isClosed = true; connected = false; cb_ = nullptr; deferred.clear(); }
void FaultyTransport::fire() {
  std::deque<TransportEvent> evs; std::function<void(const TransportEvent&)> cb;
  { std::lock_guard<std::mutex> l(m); evs.swap(deferred); cb = cb_; }
  if (cb) for (auto& e : evs) cb(e);
}
void FaultyTransport::breakLink(TransportErrorClass c, const std::string& d) {
  { std::lock_guard<std::mutex> l(m); connected = false; }
  TransportEvent e; e.type = TransportEvent::Type::Disconnected; e.cls = c; e.detail = d; emit(e);
}

// ---------------- TransportFactory ----------------
std::string TransportFactory::unsupportedReason(BroadcastProtocol p) {
  switch (p) {
    case BroadcastProtocol::RTMPS: return "RTMPS is not available: no TCP/TLS stream adapter that verifies the server certificate and host name is installed (the Windows build registers one; a qualified media worker can also provide RTMPS). Certificate validation will not be faked or skipped.";
    case BroadcastProtocol::RTMP: return "RTMP is not available: no TCP stream adapter is installed (the Windows build registers one) and no media worker is registered.";
    case BroadcastProtocol::SRT: return "no SRT stack; requires qualified media worker (FFmpeg built with libsrt, MPEG-TS H.264/AAC, caller mode); none is bundled.";
    case BroadcastProtocol::RTP: return "RTP needs a UDP socket adapter (src/platform/windows registers one); none is installed.";
  }
  return "unsupported";
}
Status TransportFactory::create(const BroadcastSettings& s, std::unique_ptr<ITransport>& out) const {
  if (override_) { out = override_(); if (out) return Status::Ok(); }
  BroadcastProtocol p = s.endpoint.protocol;
  if (workers_[(int)p]) { out = workers_[(int)p](); if (out) return Status::Ok(); return Status::Fail(Err::UnsupportedCapability, "media worker factory returned no transport"); }
  if (p == BroadcastProtocol::RTP) {
    if (!sock_) return Status::Fail(Err::UnsupportedCapability, unsupportedReason(p));
    out = makeRtpTransport(sock_, clock_ ? clock_ : &systemClock(), 0); return Status::Ok();
  }
  if (p == BroadcastProtocol::RTMP || p == BroadcastProtocol::RTMPS) {
    if (!stream_) return Status::Fail(Err::UnsupportedCapability, unsupportedReason(p), false);
    std::unique_ptr<IByteStream> bs = stream_();
    if (!bs) return Status::Fail(Err::UnsupportedCapability, "the network stream adapter could not be created", false);
    bool tls = p == BroadcastProtocol::RTMPS;
    if (tls && !bs->supportsTlsVerification())
      return Status::Fail(Err::UnsupportedCapability, "CertificateRequired: RTMPS refused because the installed network stream cannot validate the server certificate chain and host name. It will not connect unverified; use a verifying TLS adapter or a qualified media worker.", false);
    out = makeRtmpTransport(std::move(bs), tls); return Status::Ok();
  }
  return Status::Fail(Err::UnsupportedCapability, unsupportedReason(p), false);
}

} // namespace ss
