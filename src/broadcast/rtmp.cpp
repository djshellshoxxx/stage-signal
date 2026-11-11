#include "broadcast/rtmp.h"
#include "broadcast/redact.h"
#include "recording/h264.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>
namespace ss {
namespace rtmp {

// ============================== AMF0 ==============================
const Value* Value::get(const std::string& key) const {
  if (t != T::Object && t != T::EcmaArray) return nullptr;
  for (auto& p : props) if (p.key == key) return &p.val;
  return nullptr;
}
std::string Value::str(const std::string& key, const std::string& dflt) const {
  const Value* v = get(key); return v && v->t == T::String ? v->s : dflt;
}
Value& Value::set(const std::string& key, const Value& v) { Prop p; p.key = key; p.val = v; props.push_back(std::move(p)); return *this; }

static void be16(Bytes& o, uint32_t v) { o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v); }
static void be32(Bytes& o, uint32_t v) { for (int i = 3; i >= 0; --i) o.push_back((uint8_t)(v >> (8 * i))); }
static void encStr(Bytes& o, const std::string& s) { be16(o, (uint32_t)s.size()); o.insert(o.end(), s.begin(), s.end()); }
static void encProps(const Value& v, Bytes& o) {
  for (auto& p : v.props) { encStr(o, p.key); amfEncode(p.val, o); }
  o.push_back(0); o.push_back(0); o.push_back(9);
}
void amfEncode(const Value& v, Bytes& o) {
  switch (v.t) {
    case Value::T::Number: { o.push_back(0); uint64_t u; std::memcpy(&u, &v.num, 8); for (int i = 7; i >= 0; --i) o.push_back((uint8_t)(u >> (8 * i))); break; }
    case Value::T::Bool: o.push_back(1); o.push_back(v.b ? 1 : 0); break;
    case Value::T::String:
      if (v.s.size() > 0xFFFF) { o.push_back(12); be32(o, (uint32_t)v.s.size()); o.insert(o.end(), v.s.begin(), v.s.end()); }
      else { o.push_back(2); encStr(o, v.s); }
      break;
    case Value::T::Object: o.push_back(3); encProps(v, o); break;
    case Value::T::Null: o.push_back(5); break;
    case Value::T::Undefined: o.push_back(6); break;
    case Value::T::EcmaArray: o.push_back(8); be32(o, (uint32_t)v.props.size()); encProps(v, o); break;
    case Value::T::StrictArray: o.push_back(10); be32(o, (uint32_t)v.items.size()); for (auto& i : v.items) amfEncode(i, o); break;
  }
}
static bool decStr(const uint8_t* d, size_t n, size_t& p, size_t lenBytes, std::string& out) {
  if (p + lenBytes > n) return false;
  size_t l = lenBytes == 2 ? (((size_t)d[p] << 8) | d[p + 1]) : (((size_t)d[p] << 24) | ((size_t)d[p + 1] << 16) | ((size_t)d[p + 2] << 8) | d[p + 3]);
  p += lenBytes; if (l > n - p) return false;
  out.assign((const char*)d + p, l); p += l; return true;
}
static bool decProps(const uint8_t* d, size_t n, size_t& p, Value& v, int depth) {
  for (int count = 0; count < 4096; ++count) {
    if (p + 3 <= n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 9) { p += 3; return true; }
    Prop pr; if (!decStr(d, n, p, 2, pr.key)) return false;
    if (!amfDecode(d, n, p, pr.val, depth + 1)) return false;
    v.props.push_back(std::move(pr));
  }
  return false;
}
bool amfDecode(const uint8_t* d, size_t n, size_t& p, Value& out, int depth) {
  if (depth > 16 || p >= n) return false;
  uint8_t t = d[p++]; out = Value();
  switch (t) {
    case 0: if (p + 8 > n) return false; { uint64_t u = 0; for (int i = 0; i < 8; ++i) u = (u << 8) | d[p + i]; p += 8; out.t = Value::T::Number; std::memcpy(&out.num, &u, 8); } return true;
    case 1: if (p + 1 > n) return false; out.t = Value::T::Bool; out.b = d[p++] != 0; return true;
    case 2: out.t = Value::T::String; return decStr(d, n, p, 2, out.s);
    case 12: out.t = Value::T::String; return decStr(d, n, p, 4, out.s);
    case 3: out.t = Value::T::Object; return decProps(d, n, p, out, depth);
    case 5: out.t = Value::T::Null; return true;
    case 6: out.t = Value::T::Undefined; return true;
    case 8: if (p + 4 > n) return false; p += 4; out.t = Value::T::EcmaArray; return decProps(d, n, p, out, depth);
    case 10: {
      if (p + 4 > n) return false;
      uint32_t c = ((uint32_t)d[p] << 24) | (d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3]; p += 4;
      if (c > 4096 || c > n - p) return false;
      out.t = Value::T::StrictArray;
      for (uint32_t i = 0; i < c; ++i) { Value x; if (!amfDecode(d, n, p, x, depth + 1)) return false; out.items.push_back(std::move(x)); }
      return true; }
    case 11: if (p + 10 > n) return false; { uint64_t u = 0; for (int i = 0; i < 8; ++i) u = (u << 8) | d[p + i]; p += 10; out.t = Value::T::Number; std::memcpy(&out.num, &u, 8); } return true;
    default: return false;   // 4 movieclip, 7 reference, 13 unsupported, 15 xml, 16 typed object, 17 AMF3 switch: not supported
  }
}

// ============================== chunk writer ==============================
static void putBasic(Bytes& o, int fmt, uint32_t csid) {
  if (csid < 64) o.push_back((uint8_t)((fmt << 6) | csid));
  else if (csid < 320) { o.push_back((uint8_t)(fmt << 6)); o.push_back((uint8_t)(csid - 64)); }
  else { o.push_back((uint8_t)((fmt << 6) | 1)); o.push_back((uint8_t)((csid - 64) & 255)); o.push_back((uint8_t)((csid - 64) >> 8)); }
}
static void put24(Bytes& o, uint32_t v) { o.push_back((uint8_t)(v >> 16)); o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v); }

void ChunkWriter::write(uint32_t csid, uint8_t type, uint32_t sid, uint32_t ts, const uint8_t* payload, size_t n, Bytes& out) {
  if (csid < 2 || csid > 65599) return;
  if (cs_.size() <= csid) cs_.resize(csid + 1);
  Cs& c = cs_[csid];
  int fmt; uint32_t field;
  bool first = !c.valid || c.sid != sid || ts < c.ts;
  uint32_t delta = first ? 0 : ts - c.ts;
  if (first) { fmt = 0; field = ts; }
  else if (n == c.len && type == c.type) { if (c.deltaValid_ && delta == c.delta && delta < 0xFFFFFFu) { fmt = 3; field = 0; } else { fmt = 2; field = delta; } }
  else { fmt = 1; field = delta; }
  bool ext = fmt != 3 && field >= 0xFFFFFFu;
  uint32_t extVal = field;
  out.reserve(out.size() + n + 32 + (n / chunk_) * 8);
  putBasic(out, fmt, csid);
  if (fmt <= 2) put24(out, ext ? 0xFFFFFFu : field);
  if (fmt <= 1) { put24(out, (uint32_t)n); out.push_back(type); }
  if (fmt == 0) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(sid >> (8 * i))); }   // message stream id is little-endian
  if (ext) be32(out, extVal);
  size_t off = 0, first_n = std::min(n, chunk_);
  out.insert(out.end(), payload, payload + first_n); off = first_n;
  while (off < n) {
    putBasic(out, 3, csid); if (ext) be32(out, extVal);
    size_t k = std::min(chunk_, n - off); out.insert(out.end(), payload + off, payload + off + k); off += k;
  }
  c.valid = true; c.sid = sid; c.type = type; c.len = (uint32_t)n; c.ts = ts;
  if (fmt == 0) c.deltaValid_ = false; else if (fmt != 3) { c.delta = field; c.deltaValid_ = true; }
  ++stats.fmt[fmt];
}

// ============================== chunk reader ==============================
ChunkReader::Cs& ChunkReader::cs(uint32_t id) {
  for (auto& p : cs_) if (p.first == id) return p.second;
  cs_.emplace_back(id, Cs()); return cs_.back().second;
}
void ChunkReader::abort(uint32_t id) { for (auto& p : cs_) if (p.first == id) { buffered_ -= std::min(buffered_, p.second.buf.size()); p.second.buf.clear(); p.second.inMsg = false; } }

bool ChunkReader::feed(const uint8_t* d, size_t n, const std::function<void(Message&)>& on) {
  if (!error.empty()) return false;
  in_.insert(in_.end(), d, d + n);
  static const size_t mh[4] = {11, 7, 3, 0};
  for (;;) {
    size_t avail = in_.size() - pos_; if (avail < 1) break;
    const uint8_t* b = in_.data() + pos_;
    int fmt = b[0] >> 6; uint32_t id = b[0] & 63; size_t hdr = 1;
    if (id == 0) { if (avail < 2) break; id = 64 + b[1]; hdr = 2; }
    else if (id == 1) { if (avail < 3) break; id = 64 + b[1] + ((uint32_t)b[2] << 8); hdr = 3; }
    size_t need = hdr + mh[fmt]; if (avail < need) break;
    if (cs_.size() >= 512 && !std::any_of(cs_.begin(), cs_.end(), [&](const std::pair<uint32_t, Cs>& x) { return x.first == id; })) { error = "too many chunk streams"; return false; }
    Cs& c = cs(id);
    const uint8_t* h = b + hdr;
    uint32_t field = 0, len = c.len, sid = c.sid; uint8_t type = c.type;
    if (fmt <= 2) field = ((uint32_t)h[0] << 16) | (h[1] << 8) | h[2];
    if (fmt <= 1) { len = ((uint32_t)h[3] << 16) | (h[4] << 8) | h[5]; type = h[6]; }
    if (fmt == 0) sid = (uint32_t)h[7] | ((uint32_t)h[8] << 8) | ((uint32_t)h[9] << 16) | ((uint32_t)h[10] << 24);
    bool continuing = c.inMsg;
    if (fmt == 3 && !c.have) { error = "chunk with no previous header on chunk stream " + std::to_string(id); return false; }
    if (continuing && fmt != 3) { error = "new message header while a message is incomplete on chunk stream " + std::to_string(id); return false; }
    bool ext = fmt != 3 ? field == 0xFFFFFFu : c.ext;
    if (ext) { need += 4; if (avail < need) break; }
    uint32_t extVal = ext ? (((uint32_t)b[need - 4] << 24) | (b[need - 3] << 16) | (b[need - 2] << 8) | b[need - 1]) : 0;
    if (!continuing && len > kMaxIncomingMessage) { error = "incoming message too large"; return false; }
    size_t have = continuing ? c.buf.size() : 0, take = std::min<size_t>(chunk_, len - have);
    if (avail < need + take) break;
    if (!continuing) {
      uint32_t v = ext ? extVal : field, ts, delta;
      if (fmt == 0) { ts = v; delta = 0; }
      else if (fmt == 3) { delta = ext ? extVal : c.delta; ts = c.ts + delta; }
      else { delta = v; ts = c.ts + delta; }
      if (buffered_ + len > kMaxBufferedIncoming) { error = "too much incoming data buffered"; return false; }
      c.have = true; c.inMsg = true; c.len = len; c.type = type; c.sid = sid; c.ts = ts; c.delta = delta; c.ext = fmt != 3 ? ext : c.ext; c.buf.clear();
    }
    c.buf.insert(c.buf.end(), b + need, b + need + take); buffered_ += take; pos_ += need + take;
    if (c.buf.size() == c.len) {
      Message m; m.csid = id; m.ts = c.ts; m.sid = c.sid; m.type = c.type; m.payload.swap(c.buf); c.inMsg = false;
      buffered_ -= std::min(buffered_, m.payload.size());
      on(m);
      if (!error.empty()) return false;
    }
  }
  if (pos_ > 65536 || pos_ == in_.size()) { in_.erase(in_.begin(), in_.begin() + pos_); pos_ = 0; }
  return true;
}

// ============================== FLV payloads ==============================
Bytes flvAvcSequenceHeader(const Bytes& avcC) { Bytes b{0x17, 0, 0, 0, 0}; b.insert(b.end(), avcC.begin(), avcC.end()); return b; }
Bytes flvAvcNalu(bool key, const Bytes& s, int32_t cts) {
  Bytes b; b.reserve(s.size() + 5); b.push_back(key ? 0x17 : 0x27); b.push_back(1);
  b.push_back((uint8_t)(cts >> 16)); b.push_back((uint8_t)(cts >> 8)); b.push_back((uint8_t)cts); b.insert(b.end(), s.begin(), s.end()); return b;
}
Bytes flvAacSequenceHeader(const Bytes& asc) { Bytes b{0xAF, 0}; b.insert(b.end(), asc.begin(), asc.end()); return b; }
Bytes flvAacRaw(const uint8_t* a, size_t n) { Bytes b; b.reserve(n + 2); b.push_back(0xAF); b.push_back(1); b.insert(b.end(), a, a + n); return b; }
Bytes packHeaders(const Bytes& avcC, const Bytes& asc) {
  Bytes b{'S', 'S', 'H', 'D'};
  auto tlv = [&](char tag, const Bytes& v) { if (v.empty() || v.size() > 0xFFFF) return; b.push_back((uint8_t)tag); be16(b, (uint32_t)v.size()); b.insert(b.end(), v.begin(), v.end()); };
  tlv('V', avcC); tlv('A', asc); return b;
}
bool unpackHeaders(const uint8_t* d, size_t n, Bytes& avcC, Bytes& asc) {
  avcC.clear(); asc.clear();
  if (n < 4 || std::memcmp(d, "SSHD", 4) != 0) return false;
  size_t p = 4;
  while (p + 3 <= n) {
    char tag = (char)d[p]; size_t l = ((size_t)d[p + 1] << 8) | d[p + 2]; p += 3; if (l > n - p) return false;
    if (tag == 'V') avcC.assign(d + p, d + p + l); else if (tag == 'A') asc.assign(d + p, d + p + l);
    p += l;
  }
  return p == n;
}
}  // namespace rtmp

// ============================== transport ==============================
using namespace rtmp;
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kCsControl = 2, kCsCommand = 3, kCsAudio = 4, kCsData = 5, kCsVideo = 6;
void wr32(Bytes& o, uint32_t v) { for (int i = 3; i >= 0; --i) o.push_back((uint8_t)(v >> (8 * i))); }
bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }
bool contains(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }
}

struct RtmpTransport::Impl {
  std::unique_ptr<IByteStream> stream; bool tls; RtmpConfig cfg;
  mutable std::mutex m; std::function<void(const TransportEvent&)> cb;
  std::thread worker; std::atomic<bool> stop{false}, publishing{false}; bool started = false;
  std::deque<std::shared_ptr<MediaPacket>> q; size_t qBytes = 0; TransportStats st; RtmpCounters cnt;
  // parameters for the running attempt (key is wiped when the worker ends)
  BroadcastSettings settings; std::string key; Ns timeoutNs = 15 * kNsPerSec;
  // ---- worker-thread-only state ----
  struct Fail { bool set = false; bool stopped = false; TransportErrorClass cls = TransportErrorClass::Network; std::string detail; } fail;
  enum class S : uint8_t { Handshake, ConnectWait, CreateWait, PublishWait, Publishing } state = S::Handshake;
  ChunkWriter writer; ChunkReader reader; Bytes wbuf; size_t woff = 0;
  uint32_t streamId = 0, serverWindow = 2500000; uint64_t recvTotal = 0, lastAck = 0;
  Clock::time_point deadline; bool haveBase = false; Ns basePts = 0; uint32_t lastTs[2] = {0, 0};
  bool sentVideoHdr = false, sentAudioHdr = false; Bytes avcC, asc; Bytes c1;

  Impl(std::unique_ptr<IByteStream> s, bool t, const RtmpConfig& c) : stream(std::move(s)), tls(t), cfg(c) {}

  std::string scrub(const std::string& s) const { return cast::redactWithSecret(s, key); }
  void setFail(TransportErrorClass c, const std::string& d) { if (!fail.set) { fail.set = true; fail.cls = c; fail.detail = scrub(d); } }
  void emit(const TransportEvent& e) {
    std::function<void(const TransportEvent&)> f;
    { std::lock_guard<std::mutex> l(m); if (stop) return; f = cb; }
    if (f) f(e);
  }
  bool dead() const { return stop.load() || fail.set; }
  bool expired() { if (Clock::now() > deadline) { setFail(TransportErrorClass::Network, "timed out waiting for the receiver (" + phase() + ")"); return true; } return false; }
  std::string phase() const {
    switch (state) { case S::Handshake: return "handshake"; case S::ConnectWait: return "connect command"; case S::CreateWait: return "createStream"; case S::PublishWait: return "publish"; case S::Publishing: return "publishing"; }
    return "?";
  }

  // ---------------- output ----------------
  void sendMsg(uint32_t csid, uint8_t type, uint32_t sid, uint32_t ts, const Bytes& payload) {
    writer.write(csid, type, sid, ts, payload.data(), payload.size(), wbuf);
    std::lock_guard<std::mutex> l(m); ++cnt.messagesSent;
  }
  void sendCmd(uint32_t csid, uint32_t sid, const std::vector<Value>& vals) {
    Bytes b; for (auto& v : vals) amfEncode(v, b); sendMsg(csid, kCmd, sid, 0, b);
  }
  // Writes everything in wbuf; keeps reading while the peer is not accepting data (avoids two-sided buffer deadlock).
  bool flush() {
    auto lastProgress = Clock::now();
    while (woff < wbuf.size()) {
      if (dead()) return false;
      int n = stream->send(wbuf.data() + woff, std::min<size_t>(wbuf.size() - woff, 16384), 20);
      if (n < 0) { setFail(TransportErrorClass::Network, "write failed" + errText()); return false; }
      if (n > 0) { woff += (size_t)n; lastProgress = Clock::now(); std::lock_guard<std::mutex> l(m); cnt.bytesWire += (uint64_t)n; continue; }
      if (state != S::Handshake && pumpIncoming(0) < 0) return false;
      if (Clock::now() - lastProgress > std::chrono::nanoseconds(cfg.sendStallNs)) { setFail(TransportErrorClass::Network, "send stalled: the receiver stopped accepting data"); return false; }
    }
    wbuf.clear(); woff = 0; return true;
  }
  std::string errText() const { std::string e = stream->lastError(); return e.empty() ? std::string() : ": " + e; }

  // ---------------- input ----------------
  int pumpIncoming(int timeoutMs) {
    uint8_t buf[16384];
    int n = stream->recv(buf, sizeof buf, timeoutMs);
    if (n < 0) { setFail(TransportErrorClass::Network, "connection closed by the receiver" + errText()); return -1; }
    if (n == 0) return 0;
    recvTotal += (uint64_t)n; { std::lock_guard<std::mutex> l(m); cnt.bytesReceived += (uint64_t)n; }
    if (!reader.feed(buf, (size_t)n, [this](Message& msg) { onMessage(msg); })) { setFail(TransportErrorClass::Network, "protocol error: " + reader.error); return -1; }
    if (recvTotal - lastAck >= serverWindow) {   // acknowledge received bytes
      Bytes a; wr32(a, (uint32_t)recvTotal); sendMsg(kCsControl, kAck, 0, 0, a); lastAck = recvTotal;
      std::lock_guard<std::mutex> l(m); ++cnt.acksSent;
    }
    return fail.set ? -1 : 1;
  }

  struct Class { TransportErrorClass cls; std::string text; };
  static Class classify(const std::string& code, const std::string& desc) {
    std::string c = lower(code), d = lower(desc);
    std::string text = code + (desc.empty() ? "" : " (" + desc + ")");
    if (contains(c, "invalidapp")) return {TransportErrorClass::InvalidConfig, "application not found on the server: " + text};
    if (contains(c, "rejected") || contains(c, "denied") || contains(c, "unauthorized") || contains(c, "forbidden") || contains(c, "authfail") || contains(c, "auth")
        || contains(d, "unauthorized") || contains(d, "authenticat") || contains(d, "forbidden") || contains(d, "not authorized") || contains(d, "invalid stream key") || contains(d, "invalid key"))
      return {TransportErrorClass::Auth, "receiver refused the credentials: " + text};
    if (contains(c, "badname")) return {TransportErrorClass::Receiver, "stream name rejected or already in use (will retry): " + text};
    return {TransportErrorClass::Receiver, "receiver reported: " + text};
  }

  void onMessage(Message& msg) {
    const Bytes& p = msg.payload;
    switch (msg.type) {
      case kSetChunkSize: if (p.size() >= 4) { uint32_t v = (((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]) & 0x7FFFFFFFu; if (v >= 1 && v <= kMaxChunkSize) { reader.setChunkSize(v); std::lock_guard<std::mutex> l(m); ++cnt.serverChunkSizeChanges; } else setFail(TransportErrorClass::Network, "protocol error: invalid chunk size from receiver"); } break;
      case kAbort: if (p.size() >= 4) reader.abort(((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]); break;
      case kWindowAck: if (p.size() >= 4) { uint32_t w = ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; serverWindow = w ? w : 1; } break;
      case kPeerBandwidth: if (p.size() >= 4) { Bytes a(p.begin(), p.begin() + 4); sendMsg(kCsControl, kWindowAck, 0, 0, a); } break;
      case kUserControl:
        if (p.size() >= 6 && p[0] == 0 && p[1] == 6) { Bytes r{0, 7, p[2], p[3], p[4], p[5]}; sendMsg(kCsControl, kUserControl, 0, 0, r); std::lock_guard<std::mutex> l(m); ++cnt.pingsAnswered; }
        break;
      case kCmd: case kCmdAmf3: onCommand(msg); break;
      default: break;   // audio/video/data/ack from the receiver: nothing to do for a publisher
    }
  }

  void onCommand(const Message& msg) {
    size_t pos = msg.type == kCmdAmf3 ? 1 : 0; const Bytes& p = msg.payload;
    if (pos > p.size()) return;
    Value name, txn; if (!amfDecode(p.data(), p.size(), pos, name) || name.t != Value::T::String) return;
    if (!amfDecode(p.data(), p.size(), pos, txn) || txn.t != Value::T::Number) return;
    std::vector<Value> args; while (pos < p.size() && args.size() < 8) { Value v; if (!amfDecode(p.data(), p.size(), pos, v)) break; args.push_back(std::move(v)); }
    auto info = [&](size_t i) -> const Value* { return i < args.size() && args[i].t == Value::T::Object ? &args[i] : nullptr; };
    if (name.s == "_result") {
      if (state == S::ConnectWait && txn.num == 1) {
        const Value* inf = info(1); std::string code = inf ? inf->str("code") : std::string();
        if (!code.empty() && code != "NetConnection.Connect.Success") { Class c = classify(code, inf->str("description")); setFail(c.cls, c.text); return; }
        sendCmd(kCsCommand, 0, {Value::Str("releaseStream"), Value::Num(2), Value::Null(), Value::Str(key)});
        sendCmd(kCsCommand, 0, {Value::Str("FCPublish"), Value::Num(3), Value::Null(), Value::Str(key)});
        sendCmd(kCsCommand, 0, {Value::Str("createStream"), Value::Num(4), Value::Null()});
        state = S::CreateWait;
      } else if (state == S::CreateWait && txn.num == 4) {
        if (args.size() < 2 || args[1].t != Value::T::Number || args[1].num < 1 || args[1].num > 65535) { setFail(TransportErrorClass::Receiver, "createStream returned no usable stream id"); return; }
        streamId = (uint32_t)args[1].num;
        sendCmd(kCsData, streamId, {Value::Str("publish"), Value::Num(0), Value::Null(), Value::Str(key), Value::Str("live")});
        state = S::PublishWait;
      }
    } else if (name.s == "_error") {
      if (txn.num == 2 || txn.num == 3) return;   // releaseStream / FCPublish are optional on many servers
      if (state == S::ConnectWait || state == S::CreateWait) {
        const Value* inf = info(1);
        Class c = inf ? classify(inf->str("code"), inf->str("description")) : Class{TransportErrorClass::Receiver, std::string("receiver returned an error to ") + (state == S::ConnectWait ? "connect" : "createStream")};
        setFail(c.cls, c.text);
      }
    } else if (name.s == "onStatus") {
      const Value* inf = info(1); if (!inf) return;
      std::string code = inf->str("code"), level = inf->str("level"), desc = inf->str("description");
      if (state == S::PublishWait) {
        if (code == "NetStream.Publish.Start") { sendMetadata(); state = S::Publishing; }
        else if (level == "error" || startsWith(code, "NetStream.Publish.") || contains(code, "Failed")) { Class c = classify(code, desc); setFail(c.cls, c.text); }
      } else if (state == S::Publishing) {
        if (level == "error" || startsWith(code, "NetStream.Unpublish") || startsWith(code, "NetConnection.Connect.Closed") || (startsWith(code, "NetStream.Publish.") && code != "NetStream.Publish.Start")) {
          Class c = classify(code, desc); c.cls = isTerminal(c.cls) ? c.cls : TransportErrorClass::Receiver; setFail(c.cls, "the receiver ended the publish: " + c.text);
        }
      }
    }
  }

  void sendMetadata() {
    Value o = Value::Ecma();
    o.set("duration", Value::Num(0)).set("width", Value::Num(settings.width)).set("height", Value::Num(settings.height))
     .set("videodatarate", Value::Num(settings.videoKbps)).set("framerate", Value::Num(settings.fps)).set("videocodecid", Value::Num(7))
     .set("audiodatarate", Value::Num(settings.audioKbps)).set("audiosamplerate", Value::Num(settings.audioRate)).set("audiosamplesize", Value::Num(16))
     .set("stereo", Value::Bool(settings.audioChannels > 1)).set("audiocodecid", Value::Num(10)).set("encoder", Value::Str("StageSignal"));
    Bytes b; amfEncode(Value::Str("@setDataFrame"), b); amfEncode(Value::Str("onMetaData"), b); amfEncode(o, b);
    sendMsg(kCsData, kData, streamId, 0, b);
  }

  // ---------------- media ----------------
  uint32_t tsFor(Ns pts, int kind) {
    if (!haveBase) { haveBase = true; basePts = pts; }
    Ns rel = (pts - basePts) / kNsPerMs; if (rel < 0) rel = 0;
    uint32_t t = (uint32_t)(rel & 0xFFFFFFFFLL);
    if (t < lastTs[kind] && lastTs[kind] - t < 0x80000000u) t = lastTs[kind];   // never step backwards (wrap-around still allowed)
    lastTs[kind] = t; return t;
  }
  void sendVideoHeader(const Bytes& c) { if (c.empty()) return; avcC = c; sendMsg(kCsVideo, kVideo, streamId, 0, flvAvcSequenceHeader(avcC)); sentVideoHdr = true; }
  void sendAudioHeader(const Bytes& c) { if (c.empty()) return; asc = c; sendMsg(kCsAudio, kAudio, streamId, 0, flvAacSequenceHeader(asc)); sentAudioHdr = true; }
  void countDrop() { std::lock_guard<std::mutex> l(m); ++cnt.droppedPackets; ++st.packetsRejected; }
  void countUnsupported() { std::lock_guard<std::mutex> l(m); ++cnt.unsupportedPackets; }

  bool handlePacket(const MediaPacket& pk) {   // true when something was queued for the wire
    if (pk.kind == PacketKind::Headers) {
      Bytes v, a;
      if (pk.codec == PacketCodec::H264) v = pk.data; else if (pk.codec == PacketCodec::Aac) a = pk.data; else if (!unpackHeaders(pk.data.data(), pk.data.size(), v, a)) { countUnsupported(); return false; }
      sendVideoHeader(v); sendAudioHeader(a); return !v.empty() || !a.empty();
    }
    if (pk.kind == PacketKind::Video && pk.codec == PacketCodec::H264) {
      Bytes sample;
      if (h264::looksAnnexB(pk.data.data(), pk.data.size())) {
        if (!sentVideoHdr) { Bytes c = h264::avcCFromAnnexB(pk.data.data(), pk.data.size()); if (!c.empty()) sendVideoHeader(c); }
        if (!h264::annexBToAvcc(pk.data.data(), pk.data.size(), sample, true)) { countUnsupported(); return false; }
      } else sample = pk.data;
      if (!sentVideoHdr || sample.empty()) { countDrop(); return false; }   // no decoder configuration was ever supplied
      sendMsg(kCsVideo, kVideo, streamId, tsFor(pk.pts, 0), flvAvcNalu(pk.key, sample)); return true;
    }
    if (pk.kind == PacketKind::Audio && pk.codec == PacketCodec::Aac) {
      const uint8_t* d = pk.data.data(); size_t n = pk.data.size(); aac::stripAdts(d, n, d, n);
      if (!n) return false;
      sendMsg(kCsAudio, kAudio, streamId, tsFor(pk.pts, 1), flvAacRaw(d, n)); return true;
    }
    countUnsupported(); return false;   // RTMP carries H.264/AAC only: MJPEG / PCM packets are not forwarded
  }

  // ---------------- worker ----------------
  bool readExact(Bytes& buf, size_t want) {   // handshake bytes: accumulate until `want`
    uint8_t tmp[4096];
    while (buf.size() < want) {
      if (dead() || expired()) return false;
      int n = stream->recv(tmp, std::min(sizeof tmp, want - buf.size()), cfg.pollMs);
      if (n < 0) { setFail(TransportErrorClass::Network, "connection closed by the receiver during handshake" + errText()); return false; }
      if (n > 0) { buf.insert(buf.end(), tmp, tmp + n); recvTotal += (uint64_t)n; std::lock_guard<std::mutex> l(m); cnt.bytesReceived += (uint64_t)n; }
    }
    return true;
  }

  void session() {
    state = S::Handshake;
    if (key.empty()) { setFail(TransportErrorClass::InvalidConfig, "no stream key was supplied"); return; }
    if (tls && !stream->supportsTlsVerification()) {
      setFail(TransportErrorClass::Certificate, "RTMPS refused: this network stream cannot verify the server certificate and host name, and the stream key is never sent over an unverified connection"); return;
    }
    const Endpoint& ep = settings.endpoint; uint16_t port = effectivePort(ep);
    deadline = Clock::now() + std::chrono::nanoseconds(cfg.stepTimeoutNs ? cfg.stepTimeoutNs : timeoutNs);
    Status cs = stream->connect(ep.host, port, tls, timeoutNs);
    if (dead()) return;
    if (!cs.ok()) { setFail(stream->lastFailureWasCertificate() ? TransportErrorClass::Certificate : TransportErrorClass::Network, "could not connect: " + cs.detail); return; }
    // ---- handshake (simple) ----
    c1.assign(1537, 0); c1[0] = 3;
    { uint64_t x = (uint64_t)Clock::now().time_since_epoch().count() ^ (uint64_t)(uintptr_t)this * 0x9E3779B97F4A7C15ull;
      for (size_t i = 9; i < 1537; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; c1[i] = (uint8_t)(x >> 24); } }   // time=0, zero=0, then random (non-cryptographic)
    wbuf = c1; woff = 0; if (!flush()) return;
    Bytes hs; if (!readExact(hs, 1 + 1536)) return;
    if (hs[0] != 3) { setFail(TransportErrorClass::Network, "the receiver is not speaking RTMP version 3 (first byte " + std::to_string(hs[0]) + (hs[0] == 0x15 || hs[0] == 0x16 ? ", looks like TLS: wrong protocol/port?" : "") + ")"); return; }
    { Bytes c2(hs.begin() + 1, hs.begin() + 1 + 1536); c2[4] = c2[5] = c2[6] = c2[7] = 0; wbuf = c2; woff = 0; if (!flush()) return; }   // C2 = S1 echo (time2 = 0)
    if (!readExact(hs, 1 + 1536 + 1536)) return;
    { bool same = std::memcmp(hs.data() + 1 + 1536 + 8, c1.data() + 9, 1528) == 0; std::lock_guard<std::mutex> l(m); cnt.s2Matched = same; }   // lenient: some servers do not echo
    // ---- protocol start ----
    { Bytes a; wr32(a, (uint32_t)cfg.chunkSize); sendMsg(kCsControl, kSetChunkSize, 0, 0, a); writer.setChunkSize(cfg.chunkSize); }
    state = S::ConnectWait;
    { std::string app = ep.path; if (!app.empty() && app[0] == '/') app.erase(0, 1);
      std::string tc = std::string(tls ? "rtmps://" : "rtmp://") + ep.host + ":" + std::to_string(port) + "/" + app;
      Value o = Value::Obj(); o.set("app", Value::Str(app)).set("type", Value::Str("nonprivate")).set("flashVer", Value::Str(cfg.flashVer)).set("tcUrl", Value::Str(tc))
          .set("fpad", Value::Bool(false)).set("capabilities", Value::Num(15)).set("audioCodecs", Value::Num(3191)).set("videoCodecs", Value::Num(252)).set("videoFunction", Value::Num(1));
      sendCmd(kCsCommand, 0, {Value::Str("connect"), Value::Num(1), o}); }
    if (!flush()) return;
    while (state != S::Publishing) {
      if (dead() || expired()) return;
      if (pumpIncoming(cfg.pollMs) < 0) return;
      if (!flush()) return;
    }
    // ---- publishing ----
    if (!flush()) return;
    publishing = true;
    { TransportEvent e; e.type = TransportEvent::Type::Connected; emit(e); }
    while (!dead()) {
      std::vector<std::shared_ptr<MediaPacket>> batch;
      { std::lock_guard<std::mutex> l(m);
        for (int i = 0; i < 32 && !q.empty(); ++i) { qBytes -= std::min(qBytes, q.front()->data.size()); batch.push_back(std::move(q.front())); q.pop_front(); } }
      uint64_t pk = 0, by = 0;
      for (auto& b : batch) if (handlePacket(*b)) { ++pk; by += b->data.size(); }
      if (pumpIncoming(batch.empty() ? cfg.pollMs : 0) < 0) return;
      if (!flush()) return;
      if (pk) { std::lock_guard<std::mutex> l(m); st.packetsSent += pk; st.bytesSent += by; }
    }
  }

  void run() {
    session();
    publishing = false;
    bool wasStopped = stop.load();
    if (!wasStopped && fail.set) { TransportEvent e; e.type = TransportEvent::Type::Disconnected; e.cls = fail.cls; e.detail = fail.detail; emit(e); }
    stream->close();
    cast::wipe(key);
  }
};

RtmpTransport::RtmpTransport(std::unique_ptr<IByteStream> s, bool tls, const RtmpConfig& c) : p_(new Impl(std::move(s), tls, c)) {}
RtmpTransport::~RtmpTransport() { close(); }

Status RtmpTransport::connect(const TransportParams& p, std::function<void(const TransportEvent&)> cb) {
  Impl& I = *p_;
  if (!I.stream) return Status::Fail(Err::InvalidArgument, "RTMP transport has no byte stream", false);
  if (I.started) return Status::Fail(Err::InvalidArgument, "an RTMP transport instance is single-use; create a new one for each connection attempt");
  { std::lock_guard<std::mutex> l(I.m); I.cb = std::move(cb); I.q.clear(); I.qBytes = 0; I.st = TransportStats(); I.cnt = RtmpCounters(); }
  I.stop = false; I.publishing = false; I.fail = Impl::Fail(); I.settings = p.settings; I.key = p.secret; I.timeoutNs = p.connectTimeoutNs > 0 ? p.connectTimeoutNs : 15 * kNsPerSec;
  I.writer = ChunkWriter(); I.reader = ChunkReader(); I.wbuf.clear(); I.woff = 0; I.streamId = 0; I.serverWindow = 2500000; I.recvTotal = I.lastAck = 0;
  I.haveBase = false; I.lastTs[0] = I.lastTs[1] = 0; I.sentVideoHdr = I.sentAudioHdr = false; I.avcC.clear(); I.asc.clear();
  I.started = true;
  bool wantTls = p.settings.endpoint.protocol == BroadcastProtocol::RTMPS;
  if (wantTls != I.tls) {
    I.fail.set = true; I.fail.cls = TransportErrorClass::InvalidConfig; I.fail.detail = "transport protocol does not match the endpoint scheme";
    TransportEvent e; e.type = TransportEvent::Type::Disconnected; e.cls = I.fail.cls; e.detail = I.fail.detail; I.emit(e); return Status::Ok();
  }
  I.worker = std::thread([this] { p_->run(); });
  return Status::Ok();
}

Status RtmpTransport::send(const MediaPacket& pkt) {
  Impl& I = *p_;
  if (!I.publishing || I.stop) return Status::Fail(Err::TransportFailure, "not connected");
  auto c = std::make_shared<MediaPacket>(pkt);
  std::lock_guard<std::mutex> l(I.m);
  I.q.push_back(c); I.qBytes += c->data.size();
  auto droppable = [](const MediaPacket& x) { return x.kind != PacketKind::Headers && !(x.kind == PacketKind::Video && x.key); };
  while (I.q.size() > I.cfg.maxQueuePackets || I.qBytes > I.cfg.maxQueueBytes) {
    auto it = std::find_if(I.q.begin(), I.q.end(), [&](const std::shared_ptr<MediaPacket>& x) { return droppable(*x); });
    if (it == I.q.end()) {   // only key frames / headers queued: shed the oldest key frame rather than grow
      it = std::find_if(I.q.begin(), I.q.end(), [](const std::shared_ptr<MediaPacket>& x) { return x->kind != PacketKind::Headers; });
      if (it == I.q.end()) break;
      ++I.cnt.droppedKeyframeBlocked;
    }
    I.qBytes -= std::min(I.qBytes, (*it)->data.size()); I.q.erase(it); ++I.cnt.droppedPackets; ++I.st.packetsRejected;
  }
  I.cnt.queueHighWater = std::max<uint64_t>(I.cnt.queueHighWater, I.q.size());
  return Status::Ok();
}

void RtmpTransport::close() {
  Impl& I = *p_;
  { std::lock_guard<std::mutex> l(I.m); I.stop = true; I.cb = nullptr; }
  if (I.stream) I.stream->close();
  if (I.worker.joinable() && I.worker.get_id() != std::this_thread::get_id()) I.worker.join();
  I.publishing = false;
  { std::lock_guard<std::mutex> l(I.m); I.q.clear(); I.qBytes = 0; }
  cast::wipe(I.key);
}
TransportStats RtmpTransport::stats() const { std::lock_guard<std::mutex> l(p_->m); return p_->st; }
RtmpCounters RtmpTransport::counters() const { std::lock_guard<std::mutex> l(p_->m); RtmpCounters c = p_->cnt; c.queueDepth = p_->q.size(); return c; }
std::string RtmpTransport::describe() const { return p_->tls ? "rtmps client (TLS, certificate verified)" : "rtmp client (unencrypted)"; }

std::unique_ptr<ITransport> makeRtmpTransport(std::unique_ptr<IByteStream> s, bool tls, const RtmpConfig& c) { return std::unique_ptr<ITransport>(new RtmpTransport(std::move(s), tls, c)); }

} // namespace ss
