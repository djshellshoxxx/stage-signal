// Platform guard: Winsock on Windows, BSD sockets on macOS/Linux. No other file includes these headers.
#include "Engine/Desktop/UdpPacketSink.h"
#include <cstring>
#include <set>
#include <juce_core/juce_core.h>
#include "Engine/Desktop/DesktopCommon.h"

#if JUCE_WINDOWS
 #include <winsock2.h>
 #include <ws2tcpip.h>
 #include <iphlpapi.h>
 #pragma comment(lib, "ws2_32.lib")
 #pragma comment(lib, "iphlpapi.lib")
 using SockT = SOCKET;
 using SendLen = int;
 static constexpr SockT kBadSock = INVALID_SOCKET;
 static int lastSockErr() { return WSAGetLastError(); }
 static bool wouldBlock(int e) { return e == WSAEWOULDBLOCK; }
 static void closeSock(SockT s) { ::closesocket(s); }
#else
 #include <arpa/inet.h>
 #include <errno.h>
 #include <fcntl.h>
 #include <ifaddrs.h>
 #include <net/if.h>
 #include <netinet/in.h>
 #include <sys/socket.h>
 #include <unistd.h>
 using SockT = int;
 using SendLen = std::size_t;
 static constexpr SockT kBadSock = -1;
 static int lastSockErr() { return errno; }
 static bool wouldBlock(int e) { return e == EWOULDBLOCK || e == EAGAIN; }
 static void closeSock(SockT s) { ::close(s); }
#endif

namespace ss::app {

namespace {
bool parseIpv4(const std::string& s, in_addr& out) { return ::inet_pton(AF_INET, s.c_str(), &out) == 1; }
}

UdpPacketSink::UdpPacketSink() = default;
UdpPacketSink::~UdpPacketSink() { stop(); }

ss::Status UdpPacketSink::start() {
#if JUCE_WINDOWS
    static std::once_flag once;
    std::call_once(once, [] { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); });
#endif
    std::lock_guard<std::mutex> g(m_);
    if (started_.load()) return ss::okStatus();
    started_.store(true);
    state_ = connected_.load() ? AdapterState::Running : AdapterState::Degraded;
    lastError_ = connected_.load() ? "" : "no network interface chosen";
    return ss::okStatus();
}

void UdpPacketSink::stop() {
    std::lock_guard<std::mutex> g(m_);
    closeLocked();
    started_.store(false);
    state_ = AdapterState::Stopped; lastError_.clear();
}

AdapterHealth UdpPacketSink::health() const {
    std::lock_guard<std::mutex> g(m_);
    return {state_, lastError_};
}

void UdpPacketSink::closeLocked() {
    connected_.store(false, std::memory_order_release);
    const auto s = sock_.exchange(-1);
    if (s != -1) closeSock(static_cast<SockT>(s));
    boundIp_.clear();
}

void UdpPacketSink::fault(const std::string& why) {
    connected_.store(false, std::memory_order_release);        // forces a re-bind, which forces an explicit re-arm
    std::lock_guard<std::mutex> g(m_);
    state_ = AdapterState::Fault; lastError_ = desktop::clean(why);
}

ss::Status UdpPacketSink::openLocked(const std::string& ipv4) {
    in_addr local{};
    if (!parseIpv4(ipv4, local)) return desktop::err(ss::ErrorCategory::InvalidArgument, "invalid IPv4 address");
    SockT s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadSock) return desktop::err(ss::ErrorCategory::TransportFailure, "socket creation failed");
#if JUCE_WINDOWS
    u_long nb = 1; ::ioctlsocket(s, FIONBIO, &nb);
#else
    ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK);
  #if JUCE_MAC
    int one = 1; ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  #endif
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr = local; a.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closeSock(s);
        return desktop::err(ss::ErrorCategory::TransportFailure, "cannot bind to the chosen interface");
    }
    // Outgoing multicast leaves through the chosen interface with an explicit TTL; loopback stays off.
    ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&local), sizeof(local));
    const int ttl = ttl_.load();
  #if JUCE_WINDOWS
    DWORD t = static_cast<DWORD>(ttl);
  #else
    unsigned char t = static_cast<unsigned char>(ttl);
  #endif
    ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&t), sizeof(t));
    int bc = broadcast_.load() ? 1 : 0;
    ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bc), sizeof(bc));
    sock_.store(static_cast<std::intptr_t>(s));
    boundIp_ = ipv4;
    connected_.store(true, std::memory_order_release);
    return ss::okStatus();
}

ss::Status UdpPacketSink::bindInterface(const std::string& ipv4) {
    std::lock_guard<std::mutex> g(m_);
    closeLocked();
    ifaceGen_.fetch_add(1);
    auto st = openLocked(ipv4);
    if (!st.ok()) { state_ = AdapterState::Fault; lastError_ = st.error().detail; return st; }
    if (started_.load()) { state_ = AdapterState::Running; lastError_.clear(); }
    return ss::okStatus();
}

void UdpPacketSink::setBroadcastEnabled(bool on) {
    std::lock_guard<std::mutex> g(m_);
    broadcast_.store(on);
    const auto s = sock_.load();
    if (s != -1) { int v = on ? 1 : 0; ::setsockopt(static_cast<SockT>(s), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&v), sizeof(v)); }
}

void UdpPacketSink::setMulticastTtl(int ttl) { ttl_.store(ttl < 1 ? 1 : (ttl > 255 ? 255 : ttl)); }

ss::Status UdpPacketSink::send(const ss::lighting::Datagram& d) {
    if (!connected_.load(std::memory_order_acquire) || d.bytes.empty() || d.bytes.size() > 1472)
        return ss::makeError(ss::ErrorCategory::DeviceLost, "lighting transport not connected", ss::Recoverability::OperatorAction);
    const auto s = sock_.load();
    if (s == -1) return ss::makeError(ss::ErrorCategory::DeviceLost, "lighting transport not connected", ss::Recoverability::OperatorAction);
    sockaddr_in to{};
    to.sin_family = AF_INET; to.sin_port = htons(d.dst.port);
    std::uint32_t ip = (std::uint32_t{d.dst.ip.b[0]} << 24) | (std::uint32_t{d.dst.ip.b[1]} << 16) | (std::uint32_t{d.dst.ip.b[2]} << 8) | std::uint32_t{d.dst.ip.b[3]};
    to.sin_addr.s_addr = htonl(ip);
    const auto n = ::sendto(static_cast<SockT>(s), reinterpret_cast<const char*>(d.bytes.data()), static_cast<SendLen>(d.bytes.size()), 0,
                            reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    if (n == static_cast<decltype(n)>(d.bytes.size())) { sent_.fetch_add(1, std::memory_order_relaxed); return ss::okStatus(); }
    failed_.fetch_add(1, std::memory_order_relaxed);
    const int e = lastSockErr();
    if (wouldBlock(e)) return ss::makeError(ss::ErrorCategory::TransportFailure, "send buffer full, datagram dropped", ss::Recoverability::Retryable);
    return ss::makeError(ss::ErrorCategory::TransportFailure, "UDP send failed", ss::Recoverability::Retryable);
}

std::vector<NetworkInterfaceDesc> UdpPacketSink::listInterfaces() {
    std::vector<NetworkInterfaceDesc> v;
    std::set<std::string> seen;
#if JUCE_LINUX || JUCE_MAC
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) == 0) {
        for (auto* p = list; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            char buf[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, buf, sizeof(buf));
            if (seen.insert(buf).second) v.push_back({p->ifa_name ? p->ifa_name : "", buf, (p->ifa_flags & IFF_UP) != 0});
        }
        ::freeifaddrs(list);
    }
#else
    juce::Array<juce::IPAddress> all;
    juce::IPAddress::findAllAddresses(all, false);
    int i = 0;
    for (const auto& a : all) { const auto s = a.toString().toStdString(); if (seen.insert(s).second) v.push_back({"if" + std::to_string(i++), s, true}); }
#endif
    return v;
}

}  // namespace ss::app
