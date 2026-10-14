// Winsock UDP: IDatagramSink (MOD-10, REQ-L02) and IUdpSocketW. IPv4, non-blocking sends, broadcast strictly opt-in.
#include "win_internal.h"
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <atomic>
#include <cstring>
#include <map>

namespace ss { namespace win {
using namespace detail;
namespace {

std::mutex g_wsM; int g_wsRefs = 0;
bool wsAcquire() {
  std::lock_guard<std::mutex> l(g_wsM);
  if (g_wsRefs == 0) { WSADATA d; if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false; }
  ++g_wsRefs; return true;
}
void wsRelease() { std::lock_guard<std::mutex> l(g_wsM); if (--g_wsRefs == 0) WSACleanup(); }
struct Ws { bool ok; Ws() : ok(wsAcquire()) {} ~Ws() { if (ok) wsRelease(); } };

bool parseV4(const std::string& s, in_addr& out) {
  addrinfo hints; ZeroMemory(&hints, sizeof hints); hints.ai_family = AF_INET; hints.ai_flags = AI_NUMERICHOST; hints.ai_socktype = SOCK_DGRAM;
  addrinfo* r = nullptr;
  if (getaddrinfo(s.c_str(), nullptr, &hints, &r) != 0 || !r) return false;
  out = reinterpret_cast<sockaddr_in*>(r->ai_addr)->sin_addr; freeaddrinfo(r); return true;
}

// Host resolution with a small cache (numeric hosts are free; names are cached for 30 s).
class Resolver {
 public:
  bool resolve(const std::string& host, uint16_t port, sockaddr_in& out) {
    in_addr a;
    if (parseV4(host, a)) { fill(out, a, port); return true; }
    DWORD now = GetTickCount();
    { std::lock_guard<std::mutex> l(m_); auto it = cache_.find(host); if (it != cache_.end() && now - it->second.second < 30000) { fill(out, it->second.first, port); return true; } }
    addrinfo hints; ZeroMemory(&hints, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* r = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &r) != 0 || !r) return false;
    a = reinterpret_cast<sockaddr_in*>(r->ai_addr)->sin_addr; freeaddrinfo(r);
    { std::lock_guard<std::mutex> l(m_); if (cache_.size() > 16) cache_.clear(); cache_[host] = {a, now}; }
    fill(out, a, port); return true;
  }
 private:
  static void fill(sockaddr_in& o, const in_addr& a, uint16_t port) { ZeroMemory(&o, sizeof o); o.sin_family = AF_INET; o.sin_addr = a; o.sin_port = htons(port); }
  std::mutex m_; std::map<std::string, std::pair<in_addr, DWORD>> cache_;
};

Status wsFail(const char* what, int e) {
  if (e == WSAEACCES) return Status::Fail(Err::PermissionDenied, std::string(what) + ": not permitted (broadcast not enabled or blocked)");
  if (e == WSAEADDRNOTAVAIL) return Status::Fail(Err::InvalidArgument, std::string(what) + ": address not available on this machine");
  if (e == WSAEADDRINUSE) return Status::Fail(Err::ResourceExhausted, std::string(what) + ": address/port already in use");
  return Status::Fail(Err::TransportFailure, std::string(what) + " failed (WSA " + std::to_string(e) + ")");
}

class Sink : public IDatagramSink {
 public:
  ~Sink() override { close(); }
  Status open(const std::string& nic, bool allowBroadcast) override {
    close();
    ws_.reset(new Ws());
    if (!ws_->ok) { ws_.reset(); return Status::Fail(Err::TransportFailure, "Winsock startup failed"); }
    s_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_ == INVALID_SOCKET) { int e = WSAGetLastError(); ws_.reset(); return wsFail("socket", e); }
    in_addr nicAddr; nicAddr.s_addr = htonl(INADDR_ANY);
    if (!nic.empty() && !parseV4(nic, nicAddr)) { close(); return Status::Fail(Err::InvalidArgument, "NIC address must be a dotted IPv4 address"); }
    sockaddr_in la; ZeroMemory(&la, sizeof la); la.sin_family = AF_INET; la.sin_addr = nicAddr; la.sin_port = 0;
    if (bind(s_, (sockaddr*)&la, sizeof la) != 0) { int e = WSAGetLastError(); close(); return wsFail("bind to NIC", e); }
    if (!nic.empty()) setsockopt(s_, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&nicAddr, sizeof nicAddr);   // sACN multicast leaves via selected NIC
    BOOL bc = allowBroadcast ? TRUE : FALSE;                  // default OFF: OS then refuses broadcast destinations
    setsockopt(s_, SOL_SOCKET, SO_BROADCAST, (const char*)&bc, sizeof bc);
    int ttl = 8; setsockopt(s_, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
    u_long nb = 1; ioctlsocket(s_, FIONBIO, &nb);              // sends must never block the lighting scheduler
    sent_ = 0;
    return Status::Ok();
  }
  Status sendTo(const std::string& host, uint16_t port, const uint8_t* d, size_t n) override {
    if (s_ == INVALID_SOCKET) return Status::Fail(Err::InvalidArgument, "sink not open");
    if (!d || n == 0 || n > 65507) return Status::Fail(Err::InvalidArgument, "bad datagram size");
    sockaddr_in to;
    if (!res_.resolve(host, port, to)) return Status::Fail(Err::TransportFailure, "cannot resolve destination host");
    int r = sendto(s_, (const char*)d, (int)n, 0, (sockaddr*)&to, sizeof to);
    if (r == SOCKET_ERROR) {
      int e = WSAGetLastError();
      if (e == WSAEWOULDBLOCK) return Status::Fail(Err::ResourceExhausted, "send buffer full; datagram dropped");
      return wsFail("sendto", e);
    }
    ++sent_; return Status::Ok();
  }
  void close() override { if (s_ != INVALID_SOCKET) { closesocket(s_); s_ = INVALID_SOCKET; } ws_.reset(); }
  uint64_t datagramsSent() const override { return sent_; }
 private:
  std::unique_ptr<Ws> ws_; SOCKET s_ = INVALID_SOCKET; std::atomic<uint64_t> sent_{0}; Resolver res_;
};

class Udp : public IUdpSocketW {
 public:
  ~Udp() override { close(); }
  Status bind(const std::string& local, uint16_t port, bool reuse, bool broadcast) override {
    close();
    ws_.reset(new Ws());
    if (!ws_->ok) { ws_.reset(); return Status::Fail(Err::TransportFailure, "Winsock startup failed"); }
    s_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_ == INVALID_SOCKET) { int e = WSAGetLastError(); ws_.reset(); return wsFail("socket", e); }
    if (reuse) { BOOL one = TRUE; setsockopt(s_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one); }
    if (broadcast) { BOOL one = TRUE; setsockopt(s_, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one); }
    in_addr a; a.s_addr = htonl(INADDR_ANY);
    if (!local.empty() && !parseV4(local, a)) { close(); return Status::Fail(Err::InvalidArgument, "local address must be dotted IPv4"); }
    sockaddr_in la; ZeroMemory(&la, sizeof la); la.sin_family = AF_INET; la.sin_addr = a; la.sin_port = htons(port);
    if (::bind(s_, (sockaddr*)&la, sizeof la) != 0) { int e = WSAGetLastError(); close(); return wsFail("bind", e); }
    int len = sizeof la;
    if (getsockname(s_, (sockaddr*)&la, &len) == 0) port_ = ntohs(la.sin_port);
    return Status::Ok();
  }
  Status joinMulticast(const std::string& group, const std::string& nic) override {
    if (s_ == INVALID_SOCKET) return Status::Fail(Err::InvalidArgument, "socket not bound");
    ip_mreq m; ZeroMemory(&m, sizeof m);
    if (!parseV4(group, m.imr_multiaddr)) return Status::Fail(Err::InvalidArgument, "bad multicast group");
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    if (!nic.empty() && !parseV4(nic, m.imr_interface)) return Status::Fail(Err::InvalidArgument, "bad NIC address");
    if (setsockopt(s_, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof m) != 0) return wsFail("join multicast", WSAGetLastError());
    return Status::Ok();
  }
  Status sendTo(const std::string& host, uint16_t port, const uint8_t* d, size_t n) override {
    if (s_ == INVALID_SOCKET) return Status::Fail(Err::InvalidArgument, "socket not bound");
    if (!d || !n || n > 65507) return Status::Fail(Err::InvalidArgument, "bad datagram size");
    sockaddr_in to; if (!res_.resolve(host, port, to)) return Status::Fail(Err::TransportFailure, "cannot resolve destination host");
    if (sendto(s_, (const char*)d, (int)n, 0, (sockaddr*)&to, sizeof to) == SOCKET_ERROR) return wsFail("sendto", WSAGetLastError());
    return Status::Ok();
  }
  int recvFrom(uint8_t* buf, size_t cap, int timeoutMs, std::string* fromHost, uint16_t* fromPort) override {
    if (s_ == INVALID_SOCKET) return -1;
    fd_set fs; FD_ZERO(&fs); FD_SET(s_, &fs);
    timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
    int r = select(0, &fs, nullptr, nullptr, &tv);
    if (r == 0) return 0;
    if (r < 0) return -1;
    sockaddr_in from; int fl = sizeof from;
    int n = recvfrom(s_, (char*)buf, (int)cap, 0, (sockaddr*)&from, &fl);
    if (n < 0) { int e = WSAGetLastError(); return e == WSAEMSGSIZE ? (int)cap : -1; }
    if (fromHost) { char b[INET_ADDRSTRLEN] = {0}; const char* p = inet_ntoa(from.sin_addr); if (p) std::strncpy(b, p, sizeof b - 1); *fromHost = b; }
    if (fromPort) *fromPort = ntohs(from.sin_port);
    return n;
  }
  uint16_t localPort() const override { return port_; }
  void close() override { if (s_ != INVALID_SOCKET) { closesocket(s_); s_ = INVALID_SOCKET; } ws_.reset(); port_ = 0; }
 private:
  std::unique_ptr<Ws> ws_; SOCKET s_ = INVALID_SOCKET; uint16_t port_ = 0; Resolver res_;
};

} // namespace

std::unique_ptr<IDatagramSink> makeDatagramSink() { return std::unique_ptr<IDatagramSink>(new Sink()); }
std::unique_ptr<IUdpSocketW> makeUdpSocket() { return std::unique_ptr<IUdpSocketW>(new Udp()); }

std::vector<NicInfo> listNics() {
  std::vector<NicInfo> out;
  ULONG sz = 0;
  GetAdaptersInfo(nullptr, &sz);          // IPv4 adapters; present on every Windows since 98
  if (!sz) return out;
  std::vector<uint8_t> buf(sz);
  IP_ADAPTER_INFO* ai = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());
  if (GetAdaptersInfo(ai, &sz) != NO_ERROR) return out;
  for (; ai; ai = ai->Next)
    for (IP_ADDR_STRING* ip = &ai->IpAddressList; ip; ip = ip->Next) {
      if (!ip->IpAddress.String[0] || std::strcmp(ip->IpAddress.String, "0.0.0.0") == 0) continue;
      NicInfo n; n.name = ai->Description; n.ipv4 = ip->IpAddress.String; n.netmask = ip->IpMask.String; n.up = true;
      out.push_back(n);
    }
  return out;
}

}} // namespace
