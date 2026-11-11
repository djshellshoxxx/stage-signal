// MOD-12 RTMP/RTMPS client tests against an IN-PROCESS MOCK SERVER (no sockets). The mock has its own chunk parser and AMF0 codec
// (independent of src/broadcast/rtmp.cpp) so chunking, handshake and message bytes are validated against a second implementation.
// This proves the client against the mock and the written spec only; it says nothing about live receivers.
#include "test.h"
#include "broadcast/session.h"
#include "broadcast/rtmp.h"
#include "broadcast/redact.h"
#include "recording/h264.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
using namespace ss;
using Clk = std::chrono::steady_clock;
using B = std::vector<uint8_t>;
static const char* kKey = "sk_live_TOPSECRET_9f3a77";

// ------------------------------------------------------------------ pipe + stream
struct Pipe {
  std::mutex m; std::condition_variable cv; std::deque<uint8_t> q; bool closed = false; size_t cap = 1u << 22;
  int write(const uint8_t* d, size_t n, int timeoutMs) {
    std::unique_lock<std::mutex> l(m);
    cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return closed || q.size() < cap; });
    if (closed) return -1;
    if (q.size() >= cap) return 0;
    size_t k = std::min(n, cap - q.size()); q.insert(q.end(), d, d + k); cv.notify_all(); return (int)k;
  }
  int read(uint8_t* b, size_t c, int timeoutMs) {
    std::unique_lock<std::mutex> l(m);
    cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return closed || !q.empty(); });
    if (!q.empty()) { size_t k = std::min(c, q.size()); for (size_t i = 0; i < k; ++i) { b[i] = q.front(); q.pop_front(); } cv.notify_all(); return (int)k; }
    return closed ? -1 : 0;
  }
  void close() { std::lock_guard<std::mutex> l(m); closed = true; cv.notify_all(); }
};

// ------------------------------------------------------------------ independent AMF0 (test side)
struct PropA;
struct A { char t = 'N'; double n = 0; bool b = false; std::string s; std::vector<PropA> o;
  const A* get(const std::string& k) const; std::string str(const std::string& k) const; };
struct PropA { std::string k; A v; };
const A* A::get(const std::string& k) const { for (auto& p : o) if (p.k == k) return &p.v; return nullptr; }
std::string A::str(const std::string& k) const { const A* x = get(k); return x ? x->s : std::string(); }
static bool adec(const B& d, size_t& p, A& out) {
  if (p >= d.size()) return false;
  uint8_t t = d[p++];
  auto str16 = [&](std::string& s) { if (p + 2 > d.size()) return false; size_t l = (d[p] << 8) | d[p + 1]; p += 2; if (p + l > d.size()) return false; s.assign((const char*)&d[p], l); p += l; return true; };
  auto props = [&]() { for (;;) { if (p + 3 <= d.size() && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 9) { p += 3; return true; } PropA x; if (!str16(x.k)) return false; if (!adec(d, p, x.v)) return false; out.o.push_back(x); } };
  switch (t) {
    case 0: { if (p + 8 > d.size()) return false; uint64_t u = 0; for (int i = 0; i < 8; ++i) u = (u << 8) | d[p + i]; p += 8; out.t = 'N'; std::memcpy(&out.n, &u, 8); return true; }
    case 1: out.t = 'B'; out.b = d[p++] != 0; return true;
    case 2: out.t = 'S'; return str16(out.s);
    case 3: out.t = 'O'; return props();
    case 5: out.t = 'Z'; return true;
    case 6: out.t = 'U'; return true;
    case 8: p += 4; out.t = 'E'; return props();
  }
  return false;
}
static void aN(B& o, double v) { o.push_back(0); uint64_t u; std::memcpy(&u, &v, 8); for (int i = 7; i >= 0; --i) o.push_back((uint8_t)(u >> (8 * i))); }
static void aS(B& o, const std::string& s) { o.push_back(2); o.push_back((uint8_t)(s.size() >> 8)); o.push_back((uint8_t)s.size()); o.insert(o.end(), s.begin(), s.end()); }
static void aKey(B& o, const std::string& s) { o.push_back((uint8_t)(s.size() >> 8)); o.push_back((uint8_t)s.size()); o.insert(o.end(), s.begin(), s.end()); }
static void aObjEnd(B& o) { o.push_back(0); o.push_back(0); o.push_back(9); }

// ------------------------------------------------------------------ mock server
struct Scenario {
  std::string app = "live", key = kKey;
  size_t serverChunk = 128;                  // server announces Set Chunk Size when != 128
  enum Conn { ConnOk, ConnReject, ConnInvalidApp } conn = ConnOk;
  enum Pub { PubStart, PubDenied, PubBadName, PubFailedGeneric } pub = Pub::PubStart;
  bool echoKeyInDescription = false;         // server puts the stream key into its error text (must be scrubbed)
  bool silent = false;                       // never answers the handshake
  bool silentAfterHandshake = false;         // handshake ok, then never answers commands
  bool badS2 = false, badVersion = false, garbage = false;
  uint32_t windowAck = 0, filler = 0;        // window ack + filler bytes to make the client acknowledge
  bool ping = false;
  int dropAfterMedia = -1;                   // close the connection after N media messages
  bool hangConnect = false, refuseConnect = false, certFail = false;
};
struct Cmd { std::string name; double txn = 0; std::vector<A> args; uint32_t sid = 0; uint32_t csid = 0; };
struct MediaMsg { uint8_t type; uint32_t ts, sid; B payload; };
struct ServerLog {
  bool c0ok = false, c1zero = false, c2ok = false; std::vector<Cmd> cmds; std::vector<MediaMsg> media; std::vector<uint32_t> clientChunkSizes; std::vector<uint32_t> acks;
  std::vector<B> pongs; int fmtSeen[4] = {0, 0, 0, 0}; uint32_t clientWindow = 0; bool protocolError = false; std::vector<std::string> allStrings;
};
class MockServer {
 public:
  Scenario sc; Pipe c2s, s2c; ServerLog log; mutable std::mutex lm; std::atomic<bool> paused{false}, done{false}; std::thread th;
  explicit MockServer(const Scenario& s) : sc(s) {}
  ~MockServer() { shutdown(); }
  void start() { th = std::thread([this] { run(); }); }
  void shutdown() { done = true; c2s.close(); s2c.close(); if (th.joinable()) th.join(); }
  ServerLog snapshot() const { std::lock_guard<std::mutex> l(lm); return log; }
 private:
  size_t srvChunk_ = 128, cliChunk_ = 128;
  bool readN(uint8_t* b, size_t n) {
    size_t got = 0;
    while (got < n) {
      if (done) return false;
      while (paused && !done) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      int r = c2s.read(b + got, n - got, 5); if (r < 0) return false; got += (size_t)r;
    }
    return true;
  }
  void sendRaw(const B& b) { size_t off = 0; while (off < b.size() && !done) { int r = s2c.write(b.data() + off, b.size() - off, 5); if (r < 0) return; off += (size_t)r; } }
  void sendMsg(uint32_t csid, uint8_t type, uint32_t sid, uint32_t ts, const B& p) {
    B o; size_t off = 0, n = p.size();
    for (bool first = true; first || off < n; first = false) {
      o.push_back((uint8_t)((first ? 0 : 3) << 6 | csid));
      if (first) { o.push_back(ts >> 16); o.push_back(ts >> 8); o.push_back(ts); o.push_back(n >> 16); o.push_back(n >> 8); o.push_back(n); o.push_back(type); for (int i = 0; i < 4; ++i) o.push_back((uint8_t)(sid >> (8 * i))); }
      size_t k = std::min(srvChunk_, n - off); o.insert(o.end(), p.begin() + off, p.begin() + off + k); off += k; if (n == 0) break;
    }
    sendRaw(o);
  }
  void sendCmd(uint32_t csid, uint32_t sid, const B& amf) { sendMsg(csid, 20, sid, 0, amf); }
  static B u32(uint32_t v) { return B{(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; }
  void status(uint32_t sid, const std::string& level, const std::string& code, const std::string& desc, const char* cmdName = "onStatus", double txn = 0) {
    B a; aS(a, cmdName); aN(a, txn); a.push_back(5); a.push_back(3); aKey(a, "level"); aS(a, level); aKey(a, "code"); aS(a, code); aKey(a, "description"); aS(a, desc); aObjEnd(a); sendCmd(5, sid, a);
  }

  void run() {
    // ---- handshake ----
    B c01(1 + 1536);
    if (!readN(c01.data(), c01.size())) return;
    { std::lock_guard<std::mutex> l(lm); log.c0ok = c01[0] == 3; log.c1zero = c01[5] == 0 && c01[6] == 0 && c01[7] == 0 && c01[8] == 0; }
    if (sc.silent) { while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(2)); return; }
    if (sc.garbage) { B g(300, 0xC3); B hs; hs.push_back(3); for (int i = 0; i < 1536 * 2; ++i) hs.push_back((uint8_t)i); hs.insert(hs.end(), g.begin(), g.end()); sendRaw(hs); while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(2)); return; }
    B s1(1536); for (size_t i = 0; i < 1536; ++i) s1[i] = (uint8_t)(i * 7 + 3); s1[0] = s1[1] = s1[2] = s1[3] = 0; s1[4] = s1[5] = s1[6] = s1[7] = 0;
    B hs; hs.push_back(sc.badVersion ? 0x16 : 3); hs.insert(hs.end(), s1.begin(), s1.end());
    B s2(c01.begin() + 1, c01.end()); if (sc.badS2) s2[100] ^= 0xFF; hs.insert(hs.end(), s2.begin(), s2.end());
    sendRaw(hs);
    if (sc.badVersion) { while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(2)); return; }
    B c2(1536); if (!readN(c2.data(), 1536)) return;
    { std::lock_guard<std::mutex> l(lm); log.c2ok = std::memcmp(c2.data(), s1.data(), 4) == 0 && std::memcmp(c2.data() + 8, s1.data() + 8, 1528) == 0; }
    if (sc.silentAfterHandshake) { while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(2)); return; }
    // ---- chunk stream (independent blocking parser) ----
    struct St { uint32_t ts = 0, delta = 0, len = 0, sid = 0; uint8_t type = 0; bool have = false; B buf; };
    std::map<uint32_t, St> cs; int media = 0;
    for (;;) {
      uint8_t b0; if (!readN(&b0, 1)) return;
      int fmt = b0 >> 6; uint32_t id = b0 & 63;
      if (id == 0) { uint8_t x; if (!readN(&x, 1)) return; id = 64 + x; } else if (id == 1) { uint8_t x[2]; if (!readN(x, 2)) return; id = 64 + x[0] + x[1] * 256; }
      St& s = cs[id]; B h(fmt == 0 ? 11 : fmt == 1 ? 7 : fmt == 2 ? 3 : 0);
      if (!h.empty() && !readN(h.data(), h.size())) return;
      bool cont = !s.buf.empty() && s.buf.size() < s.len;
      uint32_t field = fmt <= 2 ? (h[0] << 16 | h[1] << 8 | h[2]) : 0;
      if (fmt <= 1) { s.len = h[3] << 16 | h[4] << 8 | h[5]; s.type = h[6]; }
      if (fmt == 0) s.sid = h[7] | h[8] << 8 | h[9] << 16 | (uint32_t)h[10] << 24;
      if (fmt == 3 && !s.have) { std::lock_guard<std::mutex> l(lm); log.protocolError = true; return; }
      bool ext = fmt != 3 ? field == 0xFFFFFF : false; uint32_t ev = 0;
      if (fmt == 3 && cont) { /* extended timestamp repeats only when the first header had one: tracked via s.delta sentinel */ }
      if (ext) { uint8_t e[4]; if (!readN(e, 4)) return; ev = (uint32_t)e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3]; }
      if (!cont) {
        { std::lock_guard<std::mutex> l(lm); ++log.fmtSeen[fmt]; }
        uint32_t v = ext ? ev : field;
        if (fmt == 0) { s.ts = v; s.delta = 0; } else if (fmt == 3) s.ts += s.delta; else { s.delta = v; s.ts += v; }
        s.buf.clear(); s.have = true;
        extFlag_[id] = ext;
      } else if (extFlag_[id]) { uint8_t e[4]; if (!readN(e, 4)) return; }
      size_t take = std::min<size_t>(cliChunk_, s.len - s.buf.size()); size_t at = s.buf.size(); s.buf.resize(at + take);
      if (take && !readN(s.buf.data() + at, take)) return;
      if (s.buf.size() == s.len) { B p; p.swap(s.buf); if (!handle(id, s.type, s.sid, s.ts, p, media)) return; }
    }
  }
  std::map<uint32_t, bool> extFlag_;

  bool handle(uint32_t csid, uint8_t type, uint32_t sid, uint32_t ts, const B& p, int& media) {
    if (type == 1 && p.size() >= 4) { cliChunk_ = (size_t)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]); std::lock_guard<std::mutex> l(lm); log.clientChunkSizes.push_back((uint32_t)cliChunk_); return true; }
    if (type == 3 && p.size() >= 4) { std::lock_guard<std::mutex> l(lm); log.acks.push_back((uint32_t)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3])); return true; }
    if (type == 5 && p.size() >= 4) { std::lock_guard<std::mutex> l(lm); log.clientWindow = (uint32_t)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]); return true; }
    if (type == 4 && p.size() >= 6 && p[1] == 7) { std::lock_guard<std::mutex> l(lm); log.pongs.push_back(B(p.begin() + 2, p.begin() + 6)); return true; }
    if (type == 8 || type == 9 || type == 18) {
      { std::lock_guard<std::mutex> l(lm); log.media.push_back({type, ts, sid, p}); }
      if (type != 18 && sc.dropAfterMedia >= 0 && ++media >= sc.dropAfterMedia) { s2c.close(); c2s.close(); return false; }
      return true;
    }
    if (type != 20) return true;
    size_t pos = 0; A name, txn; if (!adec(p, pos, name) || !adec(p, pos, txn)) { std::lock_guard<std::mutex> l(lm); log.protocolError = true; return false; }
    Cmd c; c.name = name.s; c.txn = txn.n; c.sid = sid; c.csid = csid; while (pos < p.size()) { A a; if (!adec(p, pos, a)) break; c.args.push_back(a); }
    { std::lock_guard<std::mutex> l(lm); log.cmds.push_back(c); }
    if (c.name == "connect") {
      bool appOk = !c.args.empty() && c.args[0].str("app") == sc.app;
      if (sc.windowAck) { sendMsg(2, 5, 0, 0, u32(sc.windowAck)); B bw = u32(sc.windowAck); bw.push_back(2); sendMsg(2, 6, 0, 0, bw); }
      if (sc.serverChunk != 128) { sendMsg(2, 1, 0, 0, u32((uint32_t)sc.serverChunk)); srvChunk_ = sc.serverChunk; }
      if (sc.ping) { B u{0, 6, 0xDE, 0xAD, 0xBE, 0xEF}; sendMsg(2, 4, 0, 0, u); }
      B a;
      if (sc.conn == Scenario::ConnReject || sc.conn == Scenario::ConnInvalidApp || !appOk) {
        const char* code = sc.conn == Scenario::ConnInvalidApp || !appOk ? "NetConnection.Connect.InvalidApp" : "NetConnection.Connect.Rejected";
        aS(a, "_error"); aN(a, 1); a.push_back(5); a.push_back(3); aKey(a, "level"); aS(a, "error"); aKey(a, "code"); aS(a, code); aKey(a, "description"); aS(a, "Connection refused"); aObjEnd(a);
      } else {
        aS(a, "_result"); aN(a, 1); a.push_back(3); aKey(a, "fmsVer"); aS(a, "FMS/3,0,1,123"); aKey(a, "capabilities"); aN(a, 31); aObjEnd(a);
        a.push_back(3); aKey(a, "level"); aS(a, "status"); aKey(a, "code"); aS(a, "NetConnection.Connect.Success"); aKey(a, "description"); aS(a, "Connection succeeded."); aObjEnd(a);
      }
      sendCmd(3, 0, a);
    } else if (c.name == "releaseStream") { B a; aS(a, "_result"); aN(a, c.txn); a.push_back(5); a.push_back(6); sendCmd(3, 0, a); }
    else if (c.name == "FCPublish") { status(0, "status", "NetStream.Publish.Start", "FCPublish", "onFCPublish", 0); }
    else if (c.name == "createStream") { B a; aS(a, "_result"); aN(a, c.txn); a.push_back(5); aN(a, 1); sendCmd(3, 0, a); }
    else if (c.name == "publish") {
      bool keyOk = c.args.size() >= 3 && c.args[1].s == sc.key && c.args[2].s == "live";
      std::string desc = sc.echoKeyInDescription ? "Stream " + (c.args.size() > 1 ? c.args[1].s : std::string()) + " is already publishing" : "reason";
      if (!keyOk || sc.pub == Scenario::PubDenied) status(sid, "error", "NetStream.Publish.Denied", sc.echoKeyInDescription ? desc : "Unauthorized");
      else if (sc.pub == Scenario::PubBadName) status(sid, "error", "NetStream.Publish.BadName", desc);
      else if (sc.pub == Scenario::PubFailedGeneric) status(sid, "error", "NetStream.Failed", desc);
      else {
        status(sid, "status", "NetStream.Publish.Start", "stream is now published");
        if (sc.filler) { B f; aS(f, "onBWDone"); aN(f, 0); aS(f, std::string(sc.filler, 'x')); sendMsg(3, 20, 0, 0, f); }
      }
    }
    return true;
  }
};

// ------------------------------------------------------------------ stream bound to a mock server
struct Harness {
  std::mutex m; std::vector<std::shared_ptr<MockServer>> servers; std::function<Scenario(int)> scen; bool tlsVerify = true; int made = 0;
  std::atomic<int> connectCalls{0}; std::string lastHost; uint16_t lastPort = 0; bool lastTls = false; size_t pipeCap = 1u << 22;
  std::shared_ptr<MockServer> last() { std::lock_guard<std::mutex> l(m); return servers.empty() ? nullptr : servers.back(); }
  std::shared_ptr<MockServer> at(size_t i) { std::lock_guard<std::mutex> l(m); return i < servers.size() ? servers[i] : nullptr; }
  size_t count() { std::lock_guard<std::mutex> l(m); return servers.size(); }
  std::unique_ptr<IByteStream> make();
  ~Harness() { for (auto& s : servers) s->shutdown(); }
};
struct MemStream : IByteStream {
  Harness& h; std::shared_ptr<MockServer> srv; Scenario sc; int index; std::atomic<bool> closed{false}; std::string err; bool certFail = false;
  std::mutex cm; std::condition_variable cv;
  MemStream(Harness& hh, const Scenario& s, int i) : h(hh), sc(s), index(i) {}
  ~MemStream() override { close(); }
  Status connect(const std::string& host, uint16_t port, bool tls, Ns timeout) override {
    ++h.connectCalls; { std::lock_guard<std::mutex> l(h.m); h.lastHost = host; h.lastPort = port; h.lastTls = tls; }
    if (sc.hangConnect) { std::unique_lock<std::mutex> l(cm); cv.wait(l, [&] { return closed.load(); }); return Status::Fail(Err::Cancelled, "cancelled"); }
    if (sc.refuseConnect) { err = "connection refused"; return Status::Fail(Err::TransportFailure, "connection refused"); }
    if (sc.certFail && tls) { err = "certificate name mismatch"; certFail = true; return Status::Fail(Err::TransportFailure, "server certificate rejected: name mismatch"); }
    srv = std::make_shared<MockServer>(sc); srv->c2s.cap = h.pipeCap; srv->start();
    { std::lock_guard<std::mutex> l(h.m); h.servers.push_back(srv); }
    return Status::Ok();
  }
  bool supportsTlsVerification() const override { return h.tlsVerify; }
  int send(const uint8_t* d, size_t n, int t) override { if (closed || !srv) return -1; return srv->c2s.write(d, n, t); }
  int recv(uint8_t* b, size_t c, int t) override { if (closed || !srv) return -1; return srv->s2c.read(b, c, t); }
  void close() override { if (closed.exchange(true)) return; { std::lock_guard<std::mutex> l(cm); } cv.notify_all(); if (srv) { srv->c2s.close(); srv->s2c.close(); } }
  std::string lastError() const override { return err; }
  bool lastFailureWasCertificate() const override { return certFail; }
};
std::unique_ptr<IByteStream> Harness::make() { int i; { std::lock_guard<std::mutex> l(m); i = made++; } return std::unique_ptr<IByteStream>(new MemStream(*this, scen ? scen(i) : Scenario(), i)); }

// ------------------------------------------------------------------ helpers
static bool waitFor(const std::function<bool()>& f, int ms = 4000) { auto end = Clk::now() + std::chrono::milliseconds(ms); while (Clk::now() < end) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(1)); } return f(); }
struct Events {
  std::mutex m; std::vector<TransportEvent> v;
  std::function<void(const TransportEvent&)> cb() { return [this](const TransportEvent& e) { std::lock_guard<std::mutex> l(m); v.push_back(e); }; }
  size_t n() { std::lock_guard<std::mutex> l(m); return v.size(); }
  TransportEvent at(size_t i) { std::lock_guard<std::mutex> l(m); return v[i]; }
};
static BroadcastSettings rtmpSettings(bool tls = false) {
  BroadcastSettings b = defaultBroadcastSettings(); b.endpoint.name = "Studio ingest"; b.endpoint.protocol = tls ? BroadcastProtocol::RTMPS : BroadcastProtocol::RTMP;
  b.endpoint.host = "ingest.example.net"; b.endpoint.path = "/live"; b.endpoint.streamKeyRef = "key-main"; return b;
}
static TransportParams params(bool tls = false, const std::string& key = kKey) { TransportParams p; p.settings = rtmpSettings(tls); p.secret = key; return p; }
static const B kSps{0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x05, 0x07, 0xE8}, kPps{0x68, 0xCE, 0x3C, 0x80};
static B annexB(bool key, size_t payload, uint8_t fill) {
  B o{0, 0, 0, 1, 0x09, 0xF0};
  if (key) { o.insert(o.end(), {0, 0, 0, 1}); o.insert(o.end(), kSps.begin(), kSps.end()); o.insert(o.end(), {0, 0, 0, 1}); o.insert(o.end(), kPps.begin(), kPps.end()); }
  o.insert(o.end(), {0, 0, 1, (uint8_t)(key ? 0x65 : 0x41)}); for (size_t i = 0; i < payload; ++i) o.push_back((uint8_t)(fill + i * 13) | 0x04);   // never a start code
  return o;
}
static B avccOf(bool key, size_t payload, uint8_t fill) { B o{(uint8_t)((payload + 1) >> 24), (uint8_t)((payload + 1) >> 16), (uint8_t)((payload + 1) >> 8), (uint8_t)(payload + 1), (uint8_t)(key ? 0x65 : 0x41)}; for (size_t i = 0; i < payload; ++i) o.push_back((uint8_t)(fill + i * 13) | 0x04); return o; }
static MediaPacketPtr vpkt(bool key, Ns pts, size_t n = 300, uint8_t fill = 1) { auto p = std::make_shared<MediaPacket>(); p->kind = PacketKind::Video; p->codec = PacketCodec::H264; p->key = key; p->pts = pts; p->data = annexB(key, n, fill); return p; }
static MediaPacketPtr apkt(Ns pts, size_t n = 200) { auto p = std::make_shared<MediaPacket>(); p->kind = PacketKind::Audio; p->codec = PacketCodec::Aac; p->key = true; p->pts = pts; p->data.assign(n, 0x21); return p; }
static B kAvcC() { return h264::buildAvcC(kSps, kPps); }
static B kAsc() { return aac::audioSpecificConfig(48000, 2); }
static MediaPacketPtr hdrPkt() { auto p = std::make_shared<MediaPacket>(); p->kind = PacketKind::Headers; p->codec = PacketCodec::Opaque; p->key = true; p->data = rtmp::packHeaders(kAvcC(), kAsc()); return p; }
static bool leaks(const std::string& s) { return s.find("TOPSECRET") != std::string::npos || s.find(kKey) != std::string::npos; }

// ================================================================== unit: AMF0 / FLV / headers
TEST(rtmp_amf0_roundtrip_and_hostile_input) {
  using namespace rtmp;
  Value o = Value::Obj(); o.set("app", Value::Str("live")).set("n", Value::Num(-12.5)).set("b", Value::Bool(true)).set("z", Value::Null());
  Value inner = Value::Ecma(); inner.set("k", Value::Str("v")); o.set("inner", inner);
  Value sa; sa.t = Value::T::StrictArray; sa.items.push_back(Value::Num(1)); sa.items.push_back(Value::Str("x")); o.set("arr", sa);
  Bytes b; amfEncode(Value::Str("connect"), b); amfEncode(Value::Num(1), b); amfEncode(o, b);
  size_t p = 0; Value v1, v2, v3; CHECK(amfDecode(b.data(), b.size(), p, v1) && amfDecode(b.data(), b.size(), p, v2) && amfDecode(b.data(), b.size(), p, v3)); CHECK(p == b.size());
  CHECK(v1.s == "connect" && v2.num == 1.0); CHECK(v3.str("app") == "live"); CHECK(v3.get("n")->num == -12.5); CHECK(v3.get("b")->b); CHECK(v3.get("inner")->str("k") == "v"); CHECK(v3.get("arr")->items.size() == 2);
  // known-answer bytes (spec §2.1): string "ab" = 02 0002 6162 ; number 1.0 = 00 3FF0000000000000
  Bytes k; amfEncode(Value::Str("ab"), k); amfEncode(Value::Num(1.0), k);
  const uint8_t want[] = {2, 0, 2, 'a', 'b', 0, 0x3F, 0xF0, 0, 0, 0, 0, 0, 0}; CHECK(k == Bytes(want, want + sizeof want));
  // our encoding decodes in the independent decoder as well
  { B tb(b.begin(), b.end()); size_t q = 0; A a1, a2, a3; CHECK(adec(tb, q, a1) && adec(tb, q, a2) && adec(tb, q, a3)); CHECK(a3.str("app") == "live" && a3.get("inner") && a3.get("inner")->t == 'E'); }
  // hostile: truncation at every length never crashes and never over-reads; deep nesting is refused
  for (size_t cut = 0; cut < b.size(); ++cut) { size_t q = 0; Value x; bool ok = true; while (ok && q < cut) ok = amfDecode(b.data(), cut, q, x); }
  Bytes deep(200, 3); for (size_t i = 0; i < 200; ++i) { deep.push_back(0); deep.push_back(1); deep.push_back('k'); } size_t q = 0; Value x; CHECK(!amfDecode(deep.data(), deep.size(), q, x));
  uint32_t seed = 7; for (int it = 0; it < 2000; ++it) { Bytes r(1 + it % 40); for (auto& c : r) { seed = seed * 1664525u + 1013904223u; c = (uint8_t)(seed >> 24); } size_t pp = 0; Value y; while (pp < r.size() && amfDecode(r.data(), r.size(), pp, y)) {} }
  const uint8_t bigStr[] = {2, 0xFF, 0xFF, 'x'}; q = 0; CHECK(!amfDecode(bigStr, sizeof bigStr, q, x));   // declared length beyond buffer
}

TEST(rtmp_flv_payloads_and_headers_bundle) {
  using namespace rtmp;
  Bytes sh = flvAvcSequenceHeader(kAvcC()); CHECK(sh[0] == 0x17 && sh[1] == 0 && sh[2] == 0 && sh[3] == 0 && sh[4] == 0 && Bytes(sh.begin() + 5, sh.end()) == kAvcC());
  Bytes nk = flvAvcNalu(false, Bytes{1, 2, 3}); CHECK(nk[0] == 0x27 && nk[1] == 1 && nk.size() == 8);
  Bytes ah = flvAacSequenceHeader(kAsc()); CHECK(ah[0] == 0xAF && ah[1] == 0 && ah.size() == 4);
  uint8_t raw[3] = {9, 8, 7}; Bytes ar = flvAacRaw(raw, 3); CHECK(ar[0] == 0xAF && ar[1] == 1 && ar[4] == 7);
  CHECK(kAsc() == (Bytes{0x11, 0x90}));   // AAC-LC, 48 kHz, stereo
  Bytes v, a; Bytes pk = packHeaders(kAvcC(), kAsc()); CHECK(unpackHeaders(pk.data(), pk.size(), v, a) && v == kAvcC() && a == kAsc());
  pk.pop_back(); CHECK(!unpackHeaders(pk.data(), pk.size(), v, a));
  // Annex B -> AVCC; SPS/PPS/AUD stripped; avcC round trip
  B ab = annexB(true, 50, 3); B out, sps, pps; CHECK(h264::annexBToAvcc(ab.data(), ab.size(), out, true, &sps, &pps)); CHECK(out == avccOf(true, 50, 3)); CHECK(sps == kSps && pps == kPps);
  B s2, p2; CHECK(h264::parseAvcC(kAvcC().data(), kAvcC().size(), s2, p2) && s2 == kSps && p2 == kPps);
  CHECK(h264::hasIdr(ab.data(), ab.size())); B ib = annexB(false, 10, 1); CHECK(!h264::hasIdr(ib.data(), ib.size()));
}

TEST(rtmp_chunk_stream_roundtrip_all_formats_and_sizes) {
  using namespace rtmp;
  for (size_t cs : {(size_t)1, (size_t)7, (size_t)128, (size_t)4096, (size_t)65536}) {
    ChunkWriter w; w.setChunkSize(cs); Bytes wire; std::vector<Message> sent;
    auto put = [&](uint32_t csid, uint8_t type, uint32_t sid, uint32_t ts, size_t n, uint8_t fill) { Message m; m.csid = csid; m.type = type; m.sid = sid; m.ts = ts; m.payload.assign(n, fill); for (size_t i = 0; i < n; ++i) m.payload[i] = (uint8_t)(fill + i); sent.push_back(m); w.write(csid, type, sid, ts, m.payload.data(), n, wire); };
    // exercises fmt 0 (first), fmt 1 (length changes), fmt 2 (same length, new delta), fmt 3 start (same delta), csid 2 / 70 / 400, ext timestamps, empty payload
    put(6, 9, 1, 0, 100, 1); put(6, 9, 1, 40, 100, 2); put(6, 9, 1, 80, 100, 3); put(6, 9, 1, 120, 100, 4); put(6, 9, 1, 130, 150, 5); put(6, 9, 1, 100, 10, 6);
    put(70, 8, 1, 5, 33, 7); put(400, 18, 1, 5, 0, 8); put(4, 8, 1, 20000000, 20, 9); put(4, 8, 1, 20000010, 20, 10); put(4, 8, 1, 0xFFFFFFF0u, 3000, 11); put(2, 4, 0, 0, 6, 12);
    CHECK(w.stats.fmt[0] > 0 && w.stats.fmt[1] > 0 && w.stats.fmt[2] > 0 && w.stats.fmt[3] > 0);
    for (int mode = 0; mode < 3; ++mode) {
      ChunkReader r; r.setChunkSize(cs); std::vector<Message> got; auto on = [&](Message& m) { got.push_back(m); };
      if (mode == 0) CHECK(r.feed(wire.data(), wire.size(), on));
      else { size_t step = mode == 1 ? 1 : 97; for (size_t i = 0; i < wire.size(); i += step) CHECK(r.feed(wire.data() + i, std::min(step, wire.size() - i), on)); }
      CHECK(got.size() == sent.size());
      for (size_t i = 0; i < got.size() && i < sent.size(); ++i) CHECK(got[i].csid == sent[i].csid && got[i].type == sent[i].type && got[i].sid == sent[i].sid && got[i].ts == sent[i].ts && got[i].payload == sent[i].payload);
    }
    // the independent parser inside the mock server accepts the same bytes (checked end-to-end in the publish tests)
  }
  // golden bytes: csid 3, cmd, ts 0, 5 bytes, sid 0, chunk 128  => 03 000000 000005 14 00000000 + payload
  { ChunkWriter w; Bytes o; const uint8_t pl[5] = {1, 2, 3, 4, 5}; w.write(3, 20, 0, 0, pl, 5, o); const uint8_t g[] = {3, 0, 0, 0, 0, 0, 5, 0x14, 0, 0, 0, 0, 1, 2, 3, 4, 5}; CHECK(o == Bytes(g, g + sizeof g)); }
  // chunk size change takes effect for the very next chunk; abort; malformed input
  { using namespace rtmp; ChunkWriter w; Bytes wire; Bytes a(300, 7); w.write(3, 20, 0, 0, a.data(), a.size(), wire); Bytes c4(5, 1); w.setChunkSize(1000); Bytes big(900, 2); Bytes sc{0, 0, 3, 0xE8}; w.write(2, 1, 0, 0, sc.data(), 4, wire);
    w.write(3, 20, 0, 0, big.data(), big.size(), wire);
    ChunkReader r; std::vector<Message> got; CHECK(r.feed(wire.data(), wire.size(), [&](Message& m) { got.push_back(m); if (m.type == 1) r.setChunkSize(1000); })); CHECK(got.size() == 3 && got[2].payload.size() == 900); }
  { ChunkReader r; uint8_t bad[] = {0xC3, 1, 2}; CHECK(!r.feed(bad, sizeof bad, [](Message&) {})); CHECK(!r.error.empty()); }                       // fmt 3 with no history
  { ChunkReader r; uint8_t huge[] = {3, 0, 0, 0, 0xFF, 0xFF, 0xFF, 20, 0, 0, 0, 0}; CHECK(!r.feed(huge, sizeof huge, [](Message&) {})); }          // 16 MiB message
  { ChunkReader r; uint32_t seed = 3; for (int it = 0; it < 3000; ++it) { uint8_t junk[64]; for (auto& c : junk) { seed = seed * 1664525u + 1013904223u; c = (uint8_t)(seed >> 24); } r.feed(junk, sizeof junk, [](Message&) {}); if (!r.error.empty()) r = ChunkReader(); } }
}

// ================================================================== end-to-end against the mock
static void checkCommandFlow(const ServerLog& L, const std::string& key) {
  CHECK(L.c0ok && L.c1zero && L.c2ok && !L.protocolError);
  std::vector<std::string> names; for (auto& c : L.cmds) names.push_back(c.name);
  CHECK(names == (std::vector<std::string>{"connect", "releaseStream", "FCPublish", "createStream", "publish"}));
  if (L.cmds.size() < 5) return;
  const Cmd& conn = L.cmds[0]; CHECK(conn.txn == 1 && conn.args.size() >= 1);
  CHECK(conn.args[0].str("app") == "live"); CHECK(conn.args[0].str("tcUrl") == "rtmp://ingest.example.net:1935/live"); CHECK(conn.args[0].str("type") == "nonprivate"); CHECK(!conn.args[0].str("flashVer").empty());
  CHECK(!leaks(conn.args[0].str("tcUrl")));
  CHECK(L.cmds[1].txn == 2 && L.cmds[1].args.size() >= 2 && L.cmds[1].args[1].s == key);
  CHECK(L.cmds[2].txn == 3 && L.cmds[2].args.size() >= 2 && L.cmds[2].args[1].s == key);
  CHECK(L.cmds[3].txn == 4);
  const Cmd& pub = L.cmds[4]; CHECK(pub.txn == 0 && pub.args.size() == 3 && pub.args[0].t == 'Z' && pub.args[1].s == key && pub.args[2].s == "live"); CHECK(pub.sid == 1);
}

TEST(rtmp_publish_flow_media_bytes_at_several_chunk_sizes) {
  for (size_t chunk : {(size_t)128, (size_t)1024, (size_t)4096, (size_t)65536}) {
    Harness h; Scenario sc; sc.serverChunk = chunk == 128 ? 4096 : 128; sc.windowAck = 3000; sc.filler = 5000; sc.ping = true; h.scen = [&](int) { return sc; };
    RtmpConfig cfg; cfg.chunkSize = chunk; RtmpTransport t(h.make(), false, cfg); Events ev;
    CHECK(t.connect(params(), ev.cb()).ok());
    CHECK(waitFor([&] { return ev.n() >= 1; })); CHECK(ev.n() >= 1 && ev.at(0).type == TransportEvent::Type::Connected);
    CHECK(h.lastHost == "ingest.example.net" && h.lastPort == 1935 && !h.lastTls);
    Status st = t.send(*hdrPkt()); CHECK(st.ok());
    const Ns t0 = 5 * kNsPerSec;
    std::vector<std::pair<uint8_t, Ns>> order; size_t bigN = 200000;
    for (int i = 0; i < 12; ++i) {
      Ns vp = t0 + (Ns)i * 33333333; bool key = i % 6 == 0; size_t n = (i == 6) ? bigN : 300 + i * 11;
      CHECK(t.send(*vpkt(key, vp, n, (uint8_t)i)).ok());
      if (i % 2 == 0) CHECK(t.send(*apkt(t0 + (Ns)i * 21333333 + 7, 100 + i)).ok());
    }
    auto srv = h.last();
    CHECK(waitFor([&] { return srv->snapshot().media.size() >= 2 + 1 + 12 + 6; }));
    ServerLog L = srv->snapshot();
    checkCommandFlow(L, kKey);
    CHECK(L.clientChunkSizes.size() == 1 && L.clientChunkSizes[0] == chunk);
    CHECK(L.fmtSeen[0] > 0 && L.fmtSeen[1] + L.fmtSeen[2] + L.fmtSeen[3] > 0);
    // metadata + 2 sequence headers + media
    int meta = 0; std::vector<MediaMsg> vid, aud;
    for (auto& m : L.media) { if (m.type == 18) { ++meta; size_t p = 0; A a1, a2, a3; CHECK(adec(m.payload, p, a1) && adec(m.payload, p, a2) && adec(m.payload, p, a3)); CHECK(a1.s == "@setDataFrame" && a2.s == "onMetaData"); CHECK(a3.get("width") && a3.get("width")->n == 1280 && a3.get("videocodecid")->n == 7 && a3.get("audiocodecid")->n == 10); CHECK(m.sid == 1); }
      else if (m.type == 9) vid.push_back(m); else if (m.type == 8) aud.push_back(m); }
    CHECK(meta == 1);
    CHECK(vid.size() == 13 && aud.size() == 7);
    if (vid.size() == 13 && aud.size() == 7) {
      CHECK(vid[0].payload == rtmp::flvAvcSequenceHeader(kAvcC()) && vid[0].ts == 0 && vid[0].sid == 1);
      CHECK(aud[0].payload == rtmp::flvAacSequenceHeader(kAsc()) && aud[0].ts == 0);
      for (int i = 0; i < 12; ++i) {
        bool key = i % 6 == 0; size_t n = (i == 6) ? bigN : 300 + i * 11; B expect = rtmp::flvAvcNalu(key, avccOf(key, n, (uint8_t)i));
        CHECK(vid[1 + i].payload == expect); CHECK(vid[1 + i].ts == (uint32_t)(((Ns)i * 33333333) / kNsPerMs)); CHECK(vid[1 + i].sid == 1);
      }
      for (int i = 0; i < 6; ++i) { B expect{0xAF, 1}; expect.insert(expect.end(), 100 + 2 * i, 0x21); CHECK(aud[1 + i].payload == expect); CHECK(aud[1 + i].ts == (uint32_t)(((Ns)(2 * i) * 21333333 + 7) / kNsPerMs)); }
    }
    // control traffic: server chunk size honoured, ping answered, acks sent for the window the server announced
    RtmpCounters c = t.counters();
    CHECK(c.s2Matched); CHECK(c.pingsAnswered == 1); CHECK(waitFor([&] { return !srv->snapshot().pongs.empty(); })); L = srv->snapshot();
    CHECK(L.pongs.size() == 1 && L.pongs[0] == (B{0xDE, 0xAD, 0xBE, 0xEF}));
    CHECK(c.serverChunkSizeChanges == (chunk == 128 ? 1u : 0u)); CHECK(c.acksSent >= 1); CHECK(L.acks.size() >= 1 && L.acks.back() >= 3000); CHECK(L.clientWindow == 3000);
    TransportStats ts = t.stats(); CHECK(ts.packetsSent >= 1 + 12 + 6 && ts.bytesSent > bigN);
    CHECK(!leaks(t.describe()));
    t.close(); CHECK(!t.send(*vpkt(false, 0)).ok());
  }
}

TEST(rtmp_chunk_types_on_the_wire_are_all_used_and_parse_independently) {
  Harness h; Scenario sc; h.scen = [&](int) { return sc; };
  RtmpConfig cfg; cfg.chunkSize = 4096; RtmpTransport t(h.make(), false, cfg); Events ev; t.connect(params(), ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; }));
  t.send(*hdrPkt());
  for (int i = 0; i < 60; ++i) { t.send(*vpkt(i % 30 == 0, (Ns)i * 40 * kNsPerMs, i % 7 == 0 ? 500 : 300)); t.send(*apkt((Ns)i * 40 * kNsPerMs, 128)); }
  auto srv = h.last(); CHECK(waitFor([&] { return srv->snapshot().media.size() >= 2 + 1 + 120; }));
  ServerLog L = srv->snapshot(); CHECK(!L.protocolError); CHECK(L.fmtSeen[0] >= 3); CHECK(L.fmtSeen[1] > 0 && L.fmtSeen[2] > 0 && L.fmtSeen[3] > 0);
  uint32_t prevV = 0, prevA = 0; for (auto& m : L.media) { if (m.type == 9) { CHECK(m.ts >= prevV); prevV = m.ts; } if (m.type == 8) { CHECK(m.ts >= prevA); prevA = m.ts; } }
}

TEST(rtmp_media_without_headers_derives_config_and_drops_unsupported) {
  Harness h; Scenario sc; h.scen = [&](int) { return sc; };
  RtmpTransport t(h.make(), false); Events ev; t.connect(params(), ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; }));
  t.send(*vpkt(false, 0));                                            // inter frame before any config: dropped, counted
  t.send(*vpkt(true, 40 * kNsPerMs));                                 // key frame carries SPS/PPS: config derived and sent first
  auto pcm = std::make_shared<MediaPacket>(); pcm->kind = PacketKind::Audio; pcm->codec = PacketCodec::PcmF32; pcm->data.assign(64, 0); t.send(*pcm);   // not carriable
  auto adts = std::make_shared<MediaPacket>(); adts->kind = PacketKind::Audio; adts->codec = PacketCodec::Aac; adts->data = {0xFF, 0xF1, 0x4C, 0x80, 0x01, 0x1F, 0xFC, 0xAA, 0xBB}; t.send(*adts);
  auto srv = h.last(); CHECK(waitFor([&] { return srv->snapshot().media.size() >= 4; })); ServerLog L = srv->snapshot();
  std::vector<MediaMsg> vid, aud; for (auto& m : L.media) { if (m.type == 9) vid.push_back(m); if (m.type == 8) aud.push_back(m); }
  CHECK(vid.size() == 2 && vid[0].payload == rtmp::flvAvcSequenceHeader(kAvcC()) && vid[1].payload == rtmp::flvAvcNalu(true, avccOf(true, 300, 1)));
  CHECK(aud.size() == 1 && aud[0].payload == (B{0xAF, 1, 0xAA, 0xBB}));          // ADTS header stripped
  RtmpCounters c = t.counters(); CHECK(c.droppedPackets == 1 && c.unsupportedPackets == 1);
}

TEST(rtmp_send_queue_is_bounded_and_drops_oldest_non_keyframe) {
  Harness h; h.pipeCap = 2048; Scenario sc; h.scen = [&](int) { return sc; };
  RtmpConfig cfg; cfg.maxQueuePackets = 16; cfg.sendStallNs = 60 * kNsPerSec; RtmpTransport t(h.make(), false, cfg); Events ev; t.connect(params(), ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; }));
  t.send(*hdrPkt()); auto srv = h.last();
  CHECK(waitFor([&] { return srv->snapshot().media.size() >= 3; }));        // headers + metadata arrived: pipe works
  srv->paused = true;                                                       // receiver stops reading: pipe fills, the worker blocks in write
  for (int i = 0; i < 20 && t.counters().queueDepth > 0; ++i) { t.send(*vpkt(false, 0, 4000)); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
  CHECK(waitFor([&] { t.send(*vpkt(false, 0, 4000)); return t.counters().queueDepth > 0; }, 500));
  CHECK(waitFor([&] { return t.counters().queueDepth >= 1; }));
  uint64_t droppedBefore = t.counters().droppedPackets;
  auto t0 = Clk::now();
  for (int i = 0; i < 31; ++i) t.send(*vpkt(i == 0, 1000 * kNsPerMs + (Ns)i * 10 * kNsPerMs, 4000, (uint8_t)i));
  t.send(*vpkt(true, 1400 * kNsPerMs, 4000, 77));
  for (int i = 0; i < 40; ++i) t.send(*vpkt(false, 1500 * kNsPerMs + (Ns)i * 10 * kNsPerMs, 4000, (uint8_t)(100 + i)));
  CHECK(Clk::now() - t0 < std::chrono::milliseconds(500));                   // callers are never blocked
  RtmpCounters c = t.counters(); CHECK(c.droppedPackets > droppedBefore + 40); CHECK(c.queueHighWater <= 16 + 1); CHECK(c.queueDepth <= 16);
  srv->paused = false;
  CHECK(waitFor([&] { return t.counters().queueDepth == 0; }));
  CHECK(waitFor([&] { auto m = srv->snapshot().media; return !m.empty() && m.back().ts >= 1890; }));
  ServerLog L = srv->snapshot(); int keys = 0; bool sawFirstKey = false, sawSecondKey = false;
  for (auto& m : L.media) if (m.type == 9 && m.payload.size() > 1 && m.payload[1] == 1) { if (m.payload[0] == 0x17) { ++keys; if (m.ts == 0) sawFirstKey = true; if (m.ts == 400) sawSecondKey = true; } }
  CHECK(sawFirstKey && sawSecondKey);                                          // both key frames survived 70 inter-frame drops
  t.close();
}

TEST(rtmp_rtmps_refused_without_certificate_verification) {
  // factory level: refuses before any socket, with a CertificateRequired explanation, never downgrades
  Harness h; h.tlsVerify = false; TransportFactory f; f.setByteStreamFactory([&] { return h.make(); });
  std::unique_ptr<ITransport> out; Status s = f.create(rtmpSettings(true), out);
  CHECK(s.code == Err::UnsupportedCapability && s.detail.find("CertificateRequired") != std::string::npos && !s.recoverable); CHECK(!out); CHECK(h.connectCalls == 0);
  // plain RTMP over the same stream is allowed (and labelled unencrypted)
  Status s2 = f.create(rtmpSettings(false), out); CHECK(s2.ok() && out); CHECK(out->describe().find("unencrypted") != std::string::npos); out.reset();
  // transport level (defence in depth): even if constructed directly the connect is refused with a terminal Certificate event
  { RtmpTransport t(h.make(), true); Events ev; t.connect(params(true), ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; })); TransportEvent e = ev.at(0);
    CHECK(e.type == TransportEvent::Type::Disconnected && e.cls == TransportErrorClass::Certificate && isTerminal(e.cls)); CHECK(h.connectCalls == 0); CHECK(!leaks(e.detail)); }
  // session level: terminal Error, secret store never read
  { struct Sec : ISecretStore { int gets = 0; Status put(const std::string&, const std::string&) override { return Status::Ok(); } Status get(const std::string&, std::string& o) override { ++gets; o = kKey; return Status::Ok(); } Status remove(const std::string&) override { return Status::Ok(); } } sec;
    FakeClock clk; clk.t = 100 * kNsPerSec; BroadcastDeps d; d.secrets = &sec; d.clock = &clk; d.factory = &f; BroadcastSession bs(d); UserActionGate g(&clk); UserStartToken tk; g.issue(ActionSource::UI, tk);
    Status st = bs.start(std::move(tk), rtmpSettings(true)); CHECK(st.code == Err::UnsupportedCapability); CHECK(bs.state() == BroadcastState::Error); CHECK(bs.status().errorTerminal); CHECK(sec.gets == 0); CHECK(h.connectCalls == 0); }
  // verifying stream: connects with tls=true and the real host name; a certificate failure is terminal
  { Harness hv; hv.tlsVerify = true; Scenario sc; hv.scen = [&](int) { return sc; }; RtmpTransport t(hv.make(), true); Events ev; t.connect(params(true), ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; })); CHECK(ev.at(0).type == TransportEvent::Type::Connected);
    CHECK(hv.lastTls && hv.lastHost == "ingest.example.net" && hv.lastPort == 443); CHECK(t.describe().find("certificate verified") != std::string::npos); t.close(); }
  { Harness hv; Scenario sc; sc.certFail = true; hv.scen = [&](int) { return sc; }; RtmpTransport t(hv.make(), true); Events ev; t.connect(params(true), ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; })); TransportEvent e = ev.at(0);
    CHECK(e.type == TransportEvent::Type::Disconnected && e.cls == TransportErrorClass::Certificate); CHECK(e.detail.find("certificate") != std::string::npos); }
}

// ------------------------------------------------------------------ session rig
struct FakeSecrets : ISecretStore {
  std::map<std::string, std::string> m; int gets = 0;
  Status put(const std::string& r, const std::string& s) override { m[r] = s; return Status::Ok(); }
  Status get(const std::string& r, std::string& out) override { ++gets; auto it = m.find(r); if (it == m.end()) return Status::Fail(Err::InvalidArgument, "no such reference"); out = it->second; return Status::Ok(); }
  Status remove(const std::string& r) override { m.erase(r); return Status::Ok(); }
};
struct Rig {
  FakeClock clk; FakeSecrets secrets; Harness h; TransportFactory factory; std::vector<std::string> logs; int keyframeReqs = 0; UserActionGate gate{&clk}; std::unique_ptr<BroadcastSession> s;
  Rig() {
    clk.t = 100 * kNsPerSec; clk.wall = 1767225600000LL; secrets.put("key-main", kKey);
    factory.setByteStreamFactory([this] { return h.make(); });
    BroadcastDeps d; d.secrets = &secrets; d.clock = &clk; d.factory = &factory; d.log = [this](const std::string& l) { logs.push_back(l); }; d.requestKeyframe = [this] { ++keyframeReqs; }; d.rngSeed = 7;
    s.reset(new BroadcastSession(d));
  }
  Status start(const BroadcastSettings& b) { UserStartToken t; gate.issue(ActionSource::UI, t); return s->start(std::move(t), b); }
  void step(Ns ms) { clk.advance(ms * kNsPerMs); s->tick(); }
  bool until(const std::function<bool()>& f, int ms = 4000) { return waitFor([&] { step(5); return f(); }, ms); }
  void feed(int n, Ns& pts) { for (int i = 0; i < n; ++i) { s->submit(vpkt(i % 5 == 0, pts, 200)); s->submit(apkt(pts, 100)); pts += 40 * kNsPerMs; } }
  bool anyLeak() { for (auto& l : logs) if (leaks(l)) return true; BroadcastStatus st = s->status(); return leaks(st.text) || leaks(st.error.detail) || leaks(st.endpoint); }
};

TEST(rtmp_session_goes_live_end_to_end_and_never_leaks_the_key) {
  Rig r; Scenario sc; r.h.scen = [&](int) { return sc; };
  r.s->setHeaders(hdrPkt());
  CHECK(r.start(rtmpSettings(false)).ok());
  CHECK(r.until([&] { return r.s->status().connected; }));
  Ns pts = 0; r.feed(20, pts);
  CHECK(r.until([&] { return r.s->state() == BroadcastState::Live; })); r.feed(10, pts); r.until([&] { return r.s->status().packetsSent >= 60; });
  BroadcastStatus st = r.s->status(); CHECK(st.live && st.mediaFlowing && st.packetsSent >= 40 && !st.encrypted); CHECK(st.encryption.find("UNENCRYPTED") != std::string::npos);
  auto srv = r.h.last(); CHECK(waitFor([&] { return srv->snapshot().media.size() > 40; }));
  ServerLog L = srv->snapshot(); checkCommandFlow(L, kKey); CHECK(L.media.size() > 40);
  // headers first: the first video message is the AVC sequence header, the first audio the AAC config
  bool v0 = false, a0 = false; for (auto& m : L.media) { if (m.type == 9 && !v0) { v0 = true; CHECK(m.payload == rtmp::flvAvcSequenceHeader(kAvcC())); } if (m.type == 8 && !a0) { a0 = true; CHECK(m.payload == rtmp::flvAacSequenceHeader(kAsc())); } }
  CHECK(v0 && a0); CHECK(!r.anyLeak()); CHECK(r.secrets.gets == 1);
  r.s->stop(); CHECK(r.s->state() == BroadcastState::Off); CHECK(!r.anyLeak());
  for (auto& l : r.logs) CHECK(l.find("sk_live") == std::string::npos);
}

TEST(rtmp_session_rtmps_connects_with_tls_flag_and_default_port) {
  Rig r; Scenario sc; r.h.scen = [&](int) { return sc; }; r.s->setHeaders(hdrPkt());
  CHECK(r.start(rtmpSettings(true)).ok()); CHECK(r.until([&] { return r.s->status().connected; }));
  CHECK(r.h.lastTls && r.h.lastPort == 443); BroadcastStatus st = r.s->status(); CHECK(st.encrypted); Ns pts = 0; r.feed(5, pts); CHECK(r.until([&] { return r.s->state() == BroadcastState::Live; })); r.s->stop();
  auto L = r.h.last()->snapshot(); CHECK(!L.cmds.empty() && L.cmds[0].args[0].str("tcUrl") == "rtmps://ingest.example.net:443/live");
}

TEST(rtmp_auth_certificate_and_config_failures_are_terminal) {
  struct Case { const char* name; std::function<void(Scenario&)> mod; TransportErrorClass cls; };
  std::vector<Case> cases = {
    {"connect rejected", [](Scenario& s) { s.conn = Scenario::ConnReject; }, TransportErrorClass::Auth},
    {"publish denied", [](Scenario& s) { s.pub = Scenario::PubDenied; }, TransportErrorClass::Auth},
    {"wrong key", [](Scenario& s) { s.key = "another-key"; }, TransportErrorClass::Auth},
    {"invalid app", [](Scenario& s) { s.conn = Scenario::ConnInvalidApp; }, TransportErrorClass::InvalidConfig},
    {"certificate", [](Scenario& s) { s.certFail = true; }, TransportErrorClass::Certificate}};
  for (auto& c : cases) {
    Rig r; Scenario sc; c.mod(sc); sc.echoKeyInDescription = true; r.h.scen = [&](int) { return sc; }; r.s->setHeaders(hdrPkt());
    bool tls = c.cls == TransportErrorClass::Certificate;
    r.start(rtmpSettings(tls));
    CHECK(r.until([&] { return r.s->state() == BroadcastState::Error; })); BroadcastStatus st = r.s->status();
    if (c.cls == TransportErrorClass::Certificate) { CHECK(st.state == BroadcastState::Error); }
    CHECK(st.errorTerminal); r.step(30000); r.step(30000); CHECK(r.s->state() == BroadcastState::Error);   // never retried
    CHECK(r.h.connectCalls == 1); CHECK(!r.anyLeak()); CHECK(!leaks(st.error.detail));
    if (!tls) CHECK(st.error.detail.find(isTerminal(c.cls) ? "will not be retried" : "") != std::string::npos);
    r.s->stop(); CHECK(r.s->state() == BroadcastState::Off);
  }
}

TEST(rtmp_retryable_receiver_errors_reconnect_and_scrub_echoed_keys) {
  Rig r; r.secrets.m["key-main"] = kKey; Scenario bad; bad.pub = Scenario::PubBadName; bad.echoKeyInDescription = true; Scenario good;
  r.h.scen = [&](int i) { return i == 0 ? bad : good; }; r.s->setHeaders(hdrPkt());
  CHECK(r.start(rtmpSettings(false)).ok());
  CHECK(r.until([&] { return r.s->state() == BroadcastState::Reconnecting; })); BroadcastStatus st = r.s->status(); CHECK(!st.errorTerminal); CHECK(!leaks(st.error.detail)); CHECK(!leaks(st.text));
  CHECK(r.until([&] { r.step(1000); return r.s->status().connected; }, 6000));
  Ns pts = 0; r.feed(8, pts); CHECK(r.until([&] { return r.s->state() == BroadcastState::Live; })); CHECK(r.s->status().reconnects == 1); CHECK(!r.anyLeak()); r.s->stop();
}

TEST(rtmp_server_drop_triggers_policy_reconnect_with_fresh_headers_and_keyframe_request) {
  Rig r; Scenario first; first.dropAfterMedia = 12; Scenario second; r.h.scen = [&](int i) { return i == 0 ? first : second; }; r.s->setHeaders(hdrPkt());
  CHECK(r.start(rtmpSettings(false)).ok()); CHECK(r.until([&] { return r.s->status().connected; })); int kfBefore = r.keyframeReqs;
  Ns pts = 0; r.feed(10, pts); CHECK(r.until([&] { return r.s->state() == BroadcastState::Live; }));
  for (int i = 0; i < 40 && r.s->state() == BroadcastState::Live; ++i) { r.feed(2, pts); r.step(10); }
  CHECK(r.until([&] { return r.s->state() == BroadcastState::Reconnecting; })); BroadcastStatus st = r.s->status(); CHECK(!st.errorTerminal); CHECK(st.error.detail.find("closed") != std::string::npos);
  CHECK(st.reconnectInMs >= 0 && st.reconnectInMs <= 1300);                       // first retry after 1 s (+-20%)
  CHECK(r.until([&] { r.step(500); return r.s->status().connected; }, 6000));
  CHECK(r.keyframeReqs > kfBefore);                                                // encoder asked for headers + IDR again
  r.feed(10, pts); CHECK(r.until([&] { return r.s->state() == BroadcastState::Live; })); CHECK(r.s->status().reconnects == 1);
  CHECK(r.h.count() == 2); auto L2 = r.h.at(1)->snapshot(); checkCommandFlow(L2, kKey);
  CHECK(waitFor([&] { return r.h.at(1)->snapshot().media.size() >= 6; }));
  L2 = r.h.at(1)->snapshot(); size_t firstVideo = 0; for (size_t i = 0; i < L2.media.size(); ++i) if (L2.media[i].type == 9) { firstVideo = i; break; }
  CHECK(L2.media[firstVideo].payload == rtmp::flvAvcSequenceHeader(kAvcC()));      // new connection starts with sequence headers
  bool firstFrameKey = false; for (auto& m : L2.media) if (m.type == 9 && m.payload.size() > 1 && m.payload[1] == 1) { firstFrameKey = m.payload[0] == 0x17; break; } CHECK(firstFrameKey);
  CHECK(!r.anyLeak()); r.s->stop();
}

TEST(rtmp_stop_cancels_connect_handshake_and_commands_within_one_second) {
  struct Mod { const char* name; std::function<void(Scenario&)> f; };
  std::vector<Mod> mods = {{"tcp connect hangs", [](Scenario& s) { s.hangConnect = true; }}, {"server silent in handshake", [](Scenario& s) { s.silent = true; }}, {"server silent after handshake", [](Scenario& s) { s.silentAfterHandshake = true; }}};
  for (auto& m : mods) {
    Rig r; Scenario sc; m.f(sc); r.h.scen = [&](int) { return sc; }; r.s->setHeaders(hdrPkt());
    CHECK(r.start(rtmpSettings(false)).ok()); CHECK(r.until([&] { return r.h.connectCalls >= 1; }));
    for (int i = 0; i < 5; ++i) r.step(5);
    auto t0 = Clk::now(); r.s->stop(); auto el = Clk::now() - t0;
    CHECK(el < std::chrono::milliseconds(1000)); CHECK(r.s->state() == BroadcastState::Off);
    BroadcastStatus st = r.s->status(); CHECK(st.state == BroadcastState::Off); r.step(1000); CHECK(r.s->state() == BroadcastState::Off); CHECK(!r.anyLeak());
    CHECK(r.h.connectCalls == 1);                                                  // nothing retried after Stop
  }
  // transport-level: close() directly while connecting
  { Harness h; Scenario sc; sc.hangConnect = true; h.scen = [&](int) { return sc; }; RtmpTransport t(h.make(), false); Events ev; t.connect(params(), ev.cb()); waitFor([&] { return h.connectCalls == 1; });
    auto t0 = Clk::now(); t.close(); CHECK(Clk::now() - t0 < std::chrono::milliseconds(500)); std::this_thread::sleep_for(std::chrono::milliseconds(20)); CHECK(ev.n() == 0); }   // no callback after close
}

TEST(rtmp_attempt_timeouts_and_hostile_servers_fail_cleanly) {
  { Harness h; Scenario sc; sc.silentAfterHandshake = true; h.scen = [&](int) { return sc; }; RtmpTransport t(h.make(), false); Events ev; TransportParams p = params(); p.connectTimeoutNs = 120 * kNsPerMs;
    t.connect(p, ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; })); TransportEvent e = ev.at(0); CHECK(e.type == TransportEvent::Type::Disconnected && e.cls == TransportErrorClass::Network && e.detail.find("timed out") != std::string::npos); }
  { Harness h; Scenario sc; sc.refuseConnect = true; h.scen = [&](int) { return sc; }; RtmpTransport t(h.make(), false); Events ev; t.connect(params(), ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; })); CHECK(ev.at(0).cls == TransportErrorClass::Network && ev.at(0).detail.find("refused") != std::string::npos); }
  { Harness h; Scenario sc; sc.badVersion = true; h.scen = [&](int) { return sc; }; RtmpTransport t(h.make(), false); Events ev; t.connect(params(), ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; })); CHECK(ev.at(0).cls == TransportErrorClass::Network && ev.at(0).detail.find("TLS") != std::string::npos); }
  { Harness h; Scenario sc; sc.garbage = true; h.scen = [&](int) { return sc; }; RtmpTransport t(h.make(), false); Events ev; t.connect(params(), ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; })); CHECK(ev.at(0).type == TransportEvent::Type::Disconnected && ev.at(0).detail.find("protocol error") != std::string::npos); }
  { Harness h; Scenario sc; sc.badS2 = true; h.scen = [&](int) { return sc; }; RtmpTransport t(h.make(), false); Events ev; t.connect(params(), ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; })); CHECK(ev.at(0).type == TransportEvent::Type::Connected); CHECK(!t.counters().s2Matched); }   // lenient on S2, but recorded
  { Harness h; Scenario sc; h.scen = [&](int) { return sc; }; RtmpTransport t(h.make(), false); Events ev; TransportParams p = params(false, ""); t.connect(p, ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; })); CHECK(ev.at(0).cls == TransportErrorClass::InvalidConfig); CHECK(h.connectCalls == 0); }
  { Harness h; Scenario sc; h.scen = [&](int) { return sc; }; RtmpTransport t(h.make(), false); Events ev; TransportParams p = params(true); t.connect(p, ev.cb()); CHECK(waitFor([&] { return ev.n() >= 1; })); CHECK(ev.at(0).cls == TransportErrorClass::InvalidConfig); }   // scheme/transport mismatch
  { Harness h; Scenario sc; h.scen = [&](int) { return sc; }; RtmpTransport t(h.make(), false); Events ev; t.connect(params(), ev.cb()); waitFor([&] { return ev.n() >= 1; }); CHECK(!t.connect(params(), ev.cb()).ok()); }   // single-use
  { Harness h; Scenario sc; h.scen = [&](int) { return sc; }; RtmpTransport t(h.make(), false); Status s = t.send(*vpkt(false, 0)); CHECK(!s.ok() && s.code == Err::TransportFailure); }   // not connected
}

TEST(rtmp_factory_status_and_secret_hygiene) {
  TransportFactory f; std::unique_ptr<ITransport> t;
  Status s = f.create(rtmpSettings(true), t); CHECK(s.code == Err::UnsupportedCapability && s.detail.find("media worker") != std::string::npos);
  BroadcastSettings srt = defaultBroadcastSettings(); srt.endpoint.protocol = BroadcastProtocol::SRT; srt.endpoint.host = "h.example.net"; srt.endpoint.port = 9000; srt.endpoint.path = ""; srt.endpoint.streamKeyRef = "";
  s = f.create(srt, t); CHECK(s.code == Err::UnsupportedCapability && s.detail.find("no SRT stack; requires qualified media worker") != std::string::npos);
  Harness h; f.setByteStreamFactory([&] { return h.make(); }); s = f.create(srt, t); CHECK(s.code == Err::UnsupportedCapability);   // a stream adapter does not make SRT appear
  // the key reaches only the commands: redactions of the transport-visible strings never contain it
  CHECK(!leaks(cast::redactWithSecret(std::string("publish ") + kKey + " failed", kKey)));
  // a registered media worker still takes precedence over the built-in client
  int made = 0; f.registerMediaWorker(BroadcastProtocol::RTMPS, [&] { ++made; return std::unique_ptr<ITransport>(new FaultyTransport()); }); CHECK(f.create(rtmpSettings(true), t).ok() && made == 1);
}

int main() { return ss_test::runAll(); }
