// TCP byte stream for the portable RTMP client, with optional Schannel TLS client (SNI, chain + host name validation).
// HONEST SCOPE: cross-compiled only; the TLS path has not been run against a real server in this environment (wine has no usable
// Schannel). Revocation is not checked. No certificate-error override exists. Name resolution runs on a helper thread so close()
// aborts a stalled lookup within one poll slice.
#define SECURITY_WIN32
#include "win_internal.h"
#include <ws2tcpip.h>
#include <sspi.h>
#include <schnlsp.h>
#include <wincrypt.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace ss { namespace win {
using namespace detail;
namespace {
using Clock = std::chrono::steady_clock;

struct SecApi {
  HMODULE sec = nullptr, crypt = nullptr; PSecurityFunctionTableW t = nullptr;
  BOOL (WINAPI *getChain)(HCERTCHAINENGINE, PCCERT_CONTEXT, LPFILETIME, HCERTSTORE, PCERT_CHAIN_PARA, DWORD, LPVOID, PCCERT_CHAIN_CONTEXT*) = nullptr;
  void (WINAPI *freeChain)(PCCERT_CHAIN_CONTEXT) = nullptr;
  BOOL (WINAPI *verifyPolicy)(LPCSTR, PCCERT_CHAIN_CONTEXT, PCERT_CHAIN_POLICY_PARA, PCERT_CHAIN_POLICY_STATUS) = nullptr;
  BOOL (WINAPI *freeCert)(PCCERT_CONTEXT) = nullptr;
  bool ok() const { return t && getChain && freeChain && verifyPolicy && freeCert; }
};
const SecApi& secApi() {
  static SecApi a = [] {
    SecApi x;
    x.sec = LoadLibraryW(L"secur32.dll"); x.crypt = LoadLibraryW(L"crypt32.dll");
    typedef PSecurityFunctionTableW (WINAPI *InitFn)(void);
    InitFn init = procAddr<InitFn>(x.sec, "InitSecurityInterfaceW");
    if (init) x.t = init();
    x.getChain = procAddr<decltype(x.getChain)>(x.crypt, "CertGetCertificateChain");
    x.freeChain = procAddr<decltype(x.freeChain)>(x.crypt, "CertFreeCertificateChain");
    x.verifyPolicy = procAddr<decltype(x.verifyPolicy)>(x.crypt, "CertVerifyCertificateChainPolicy");
    x.freeCert = procAddr<decltype(x.freeCert)>(x.crypt, "CertFreeCertificateContext");
    if (x.t && !(x.t->AcquireCredentialsHandleW && x.t->InitializeSecurityContextW && x.t->EncryptMessage && x.t->DecryptMessage && x.t->QueryContextAttributesW && x.t->DeleteSecurityContext && x.t->FreeCredentialsHandle && x.t->FreeContextBuffer)) x.t = nullptr;
    return x;
  }();
  return a;
}

struct Resolved {
  std::string host, port; addrinfo* res = nullptr; std::atomic<int> state{0}; int rc = 0;   // 0 running, 1 done
  ~Resolved() { if (res) freeaddrinfo(res); }
};

std::string certErrorText(DWORD e) {
  switch ((uint32_t)e) {
    case 0x800B010F: return "the certificate does not match the host name";            // CERT_E_CN_NO_MATCH
    case 0x800B0109: return "the certificate chain ends in an untrusted root";           // CERT_E_UNTRUSTEDROOT
    case 0x800B0101: return "the certificate has expired or is not yet valid";           // CERT_E_EXPIRED
    case 0x800B010A: return "the certificate chain could not be built";                  // CERT_E_CHAINING
    case 0x800B0111: return "the certificate was revoked";                               // CERT_E_REVOKED
    case 0x800B0110: return "the certificate is not valid for server authentication";    // CERT_E_WRONG_USAGE
    default: return "certificate validation failed (" + hex(e, 8) + ")";
  }
}

class TcpStream : public IByteStream {
 public:
  ~TcpStream() override { close(); teardown(); }

  bool supportsTlsVerification() const override { return secApi().ok(); }
  std::string lastError() const override { return err_; }
  bool lastFailureWasCertificate() const override { return certFail_; }

  Status connect(const std::string& host, uint16_t port, bool tls, Ns timeout) override {
    teardown(); closed_ = false; err_.clear(); certFail_ = false; tls_ = false;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_) != 0) return fail("Winsock startup failed");
    wsOn_ = true;
    if (tls && !secApi().ok()) return fail("TLS is not available on this Windows version (Schannel/crypt32 entry points missing)");
    auto end = Clock::now() + std::chrono::nanoseconds(timeout > 0 ? timeout : 15 * kNsPerSec);
    // ---- resolve (helper thread, cancellable) ----
    std::shared_ptr<Resolved> rs = std::make_shared<Resolved>(); rs->host = host; rs->port = std::to_string(port);
    std::thread([rs] { addrinfo h; ZeroMemory(&h, sizeof h); h.ai_family = AF_UNSPEC; h.ai_socktype = SOCK_STREAM; h.ai_protocol = IPPROTO_TCP;
                       addrinfo* r = nullptr; rs->rc = getaddrinfo(rs->host.c_str(), rs->port.c_str(), &h, &r); rs->res = r; rs->state = 1; }).detach();
    while (rs->state.load() == 0) { if (closed_) return fail("cancelled"); if (Clock::now() > end) return fail("timed out resolving the host name"); Sleep(10); }
    if (rs->rc != 0 || !rs->res) return fail("cannot resolve the host name");
    // ---- TCP connect, first address that answers ----
    std::string last = "connection failed";
    for (addrinfo* a = rs->res; a; a = a->ai_next) {
      if (closed_) return fail("cancelled");
      SOCKET s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol); if (s == INVALID_SOCKET) continue;
      u_long nb = 1; ioctlsocket(s, FIONBIO, &nb); BOOL one = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
      sock_ = s;
      int r = ::connect(s, a->ai_addr, (int)a->ai_addrlen);
      if (r != 0 && WSAGetLastError() != WSAEWOULDBLOCK) { last = "connection refused or unreachable (WSA " + std::to_string(WSAGetLastError()) + ")"; closesocket(s); sock_ = INVALID_SOCKET; continue; }
      int w = r == 0 ? 1 : waitSock(true, end);
      if (w == 1) { int e = 0, el = sizeof e; getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&e, &el); if (e == 0) break; last = "connection refused or unreachable (WSA " + std::to_string(e) + ")"; }
      else if (w == 0) last = "connection timed out"; else last = "cancelled";
      closesocket(s); sock_ = INVALID_SOCKET; if (closed_ || Clock::now() > end) break;
    }
    if (sock_ == INVALID_SOCKET) return fail(closed_ ? "cancelled" : last);
    if (tls) { Status s = handshake(host, end); if (!s.ok()) return s; tls_ = true; }
    return Status::Ok();
  }

  int send(const uint8_t* d, size_t n, int timeoutMs) override {
    if (closed_ || sock_ == INVALID_SOCKET) return -1;
    auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    if (!tls_) return rawSend(d, n, end);
    if (!out_.empty()) { int f = flushOut(end); if (f < 0) return -1; if (!out_.empty()) return 0; }
    size_t chunk = std::min<size_t>(n, sizes_.cbMaximumMessage); if (!chunk) return 0;
    std::vector<uint8_t> rec(sizes_.cbHeader + chunk + sizes_.cbTrailer);
    std::memcpy(rec.data() + sizes_.cbHeader, d, chunk);
    SecBuffer b[4]; b[0].BufferType = SECBUFFER_STREAM_HEADER; b[0].cbBuffer = sizes_.cbHeader; b[0].pvBuffer = rec.data();
    b[1].BufferType = SECBUFFER_DATA; b[1].cbBuffer = (ULONG)chunk; b[1].pvBuffer = rec.data() + sizes_.cbHeader;
    b[2].BufferType = SECBUFFER_STREAM_TRAILER; b[2].cbBuffer = sizes_.cbTrailer; b[2].pvBuffer = rec.data() + sizes_.cbHeader + chunk;
    b[3].BufferType = SECBUFFER_EMPTY; b[3].cbBuffer = 0; b[3].pvBuffer = nullptr;
    SecBufferDesc desc = {SECBUFFER_VERSION, 4, b};
    SECURITY_STATUS ss_ = secApi().t->EncryptMessage(&ctx_, 0, &desc, 0);
    if (ss_ != SEC_E_OK) { err_ = "TLS encrypt failed (" + hrString(ss_) + ")"; return -1; }
    out_.assign(rec.begin(), rec.begin() + (b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer));
    flushOut(end);                                      // remainder (if any) is flushed by the next send()
    return closed_ ? -1 : (int)chunk;
  }

  int recv(uint8_t* buf, size_t cap, int timeoutMs) override {
    if (closed_ || sock_ == INVALID_SOCKET) return -1;
    auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    if (!tls_) return rawRecv(buf, cap, end);
    for (;;) {
      if (!plain_.empty()) { size_t k = std::min(cap, plain_.size()); std::memcpy(buf, plain_.data(), k); plain_.erase(plain_.begin(), plain_.begin() + k); return (int)k; }
      if (!enc_.empty()) {
        SecBuffer b[4]; b[0].BufferType = SECBUFFER_DATA; b[0].cbBuffer = (ULONG)enc_.size(); b[0].pvBuffer = enc_.data();
        for (int i = 1; i < 4; ++i) { b[i].BufferType = SECBUFFER_EMPTY; b[i].cbBuffer = 0; b[i].pvBuffer = nullptr; }
        SecBufferDesc desc = {SECBUFFER_VERSION, 4, b};
        SECURITY_STATUS r = secApi().t->DecryptMessage(&ctx_, &desc, 0, nullptr);
        if (r == SEC_E_OK) {
          uint8_t* data = nullptr; size_t dl = 0, extra = 0;
          for (int i = 1; i < 4; ++i) { if (b[i].BufferType == SECBUFFER_DATA) { data = (uint8_t*)b[i].pvBuffer; dl = b[i].cbBuffer; } if (b[i].BufferType == SECBUFFER_EXTRA) extra = b[i].cbBuffer; }
          if (data && dl) plain_.insert(plain_.end(), data, data + dl);
          if (extra) enc_.erase(enc_.begin(), enc_.end() - extra); else enc_.clear();
          continue;
        }
        if (r == SEC_E_INCOMPLETE_MESSAGE) { /* need more ciphertext */ }
        else if (r == SEC_I_CONTEXT_EXPIRED) { err_ = "TLS connection closed by the server"; return -1; }
        else { err_ = "TLS decrypt failed (" + hrString(r) + ")"; return -1; }   // includes SEC_I_RENEGOTIATE (not supported)
      }
      uint8_t tmp[16384]; int n = rawRecv(tmp, sizeof tmp, end);
      if (n <= 0) return n;
      enc_.insert(enc_.end(), tmp, tmp + n);
    }
  }

  void close() override {
    if (closed_.exchange(true)) return;
    SOCKET s = sock_; if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);    // wakes select(); the handle is released in teardown()
  }

 private:
  Status fail(const std::string& m) { err_ = m; return Status::Fail(Err::TransportFailure, m); }
  Status failCert(const std::string& m) { certFail_ = true; err_ = m; return Status::Fail(Err::TransportFailure, m); }
  void teardown() {
    if (ctxOn_) { secApi().t->DeleteSecurityContext(&ctx_); ctxOn_ = false; }
    if (credOn_) { secApi().t->FreeCredentialsHandle(&cred_); credOn_ = false; }
    if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
    if (wsOn_) { WSACleanup(); wsOn_ = false; }
    enc_.clear(); plain_.clear(); out_.clear(); tls_ = false;
  }
  // 1 ready, 0 timeout, -1 closed/error. 20 ms slices so close() is noticed quickly.
  int waitSock(bool write, Clock::time_point end) {
    for (;;) {
      if (closed_) return -1;
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
      long ms = left < 0 ? 0 : (left > 20 ? 20 : (long)left);
      fd_set f; FD_ZERO(&f); FD_SET(sock_, &f); timeval tv; tv.tv_sec = 0; tv.tv_usec = ms * 1000;
      int r = write ? select(0, nullptr, &f, nullptr, &tv) : select(0, &f, nullptr, nullptr, &tv);
      if (r > 0) return 1;
      if (r < 0) return -1;
      if (left <= 0) return 0;
    }
  }
  int rawSend(const uint8_t* d, size_t n, Clock::time_point end) {
    int w = waitSock(true, end); if (w <= 0) return w;
    int r = ::send(sock_, (const char*)d, (int)std::min<size_t>(n, 1 << 20), 0);
    if (r == SOCKET_ERROR) { int e = WSAGetLastError(); if (e == WSAEWOULDBLOCK) return 0; err_ = "send failed (WSA " + std::to_string(e) + ")"; return -1; }
    return r;
  }
  int rawRecv(uint8_t* b, size_t cap, Clock::time_point end) {
    int w = waitSock(false, end); if (w <= 0) return w;
    int r = ::recv(sock_, (char*)b, (int)cap, 0);
    if (r == 0) { err_ = "connection closed by the server"; return -1; }
    if (r == SOCKET_ERROR) { int e = WSAGetLastError(); if (e == WSAEWOULDBLOCK) return 0; err_ = "receive failed (WSA " + std::to_string(e) + ")"; return -1; }
    return r;
  }
  int flushOut(Clock::time_point end) {
    while (!out_.empty()) { int n = rawSend(out_.data(), out_.size(), end); if (n < 0) return -1; if (n == 0) return 0; out_.erase(out_.begin(), out_.begin() + n); }
    return 1;
  }
  bool sendAll(const uint8_t* d, size_t n, Clock::time_point end) {
    size_t off = 0; while (off < n) { int r = rawSend(d + off, n - off, end); if (r < 0) return false; if (r == 0 && Clock::now() > end) return false; off += (size_t)r; } return true;
  }

  Status handshake(const std::string& host, Clock::time_point end) {
    const SecApi& A = secApi(); std::wstring whost = widen(host);
    SCHANNEL_CRED sc; ZeroMemory(&sc, sizeof sc); sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION;       // validation is done below, explicitly
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_1_CLIENT;       // no SSL3 / TLS 1.0
    TimeStamp ts;
    SECURITY_STATUS st = A.t->AcquireCredentialsHandleW(nullptr, const_cast<SEC_WCHAR*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr, nullptr, &cred_, &ts);
    if (st != SEC_E_OK) return fail("TLS client credentials could not be created (" + hrString(st) + "); TLS 1.1/1.2 may be unavailable on this Windows version");
    credOn_ = true;
    const DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
    DWORD attrs = 0; bool first = true; enc_.clear();
    for (;;) {
      SecBuffer ib[2], ob[1]; SecBufferDesc id = {SECBUFFER_VERSION, 2, ib}, od = {SECBUFFER_VERSION, 1, ob};
      ob[0].BufferType = SECBUFFER_TOKEN; ob[0].cbBuffer = 0; ob[0].pvBuffer = nullptr;
      ib[0].BufferType = SECBUFFER_EMPTY; ib[0].cbBuffer = 0; ib[0].pvBuffer = nullptr; ib[1] = ib[0];
      if (!first) { ib[0].BufferType = SECBUFFER_TOKEN; ib[0].cbBuffer = (ULONG)enc_.size(); ib[0].pvBuffer = enc_.data(); ib[1].BufferType = SECBUFFER_EMPTY; ib[1].cbBuffer = 0; ib[1].pvBuffer = nullptr; }
      st = A.t->InitializeSecurityContextW(&cred_, first ? nullptr : &ctx_, const_cast<SEC_WCHAR*>(whost.c_str()), flags, 0, 0, first ? nullptr : &id, 0, first ? &ctx_ : nullptr, &od, &attrs, &ts);
      if (first && (st == SEC_I_CONTINUE_NEEDED || st == SEC_E_OK)) ctxOn_ = true;
      first = false;
      if (ob[0].cbBuffer && ob[0].pvBuffer) { bool ok = sendAll((const uint8_t*)ob[0].pvBuffer, ob[0].cbBuffer, end); A.t->FreeContextBuffer(ob[0].pvBuffer); if (!ok) return fail(closed_ ? "cancelled" : "TLS handshake send failed"); }
      if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED) {
        if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer) enc_.erase(enc_.begin(), enc_.end() - ib[1].cbBuffer); else enc_.clear();
        if (st == SEC_E_OK) break;
      } else if (st != SEC_E_INCOMPLETE_MESSAGE) {
        if (st == (SECURITY_STATUS)SEC_E_UNTRUSTED_ROOT || st == (SECURITY_STATUS)SEC_E_CERT_EXPIRED || st == (SECURITY_STATUS)SEC_E_WRONG_PRINCIPAL || st == (SECURITY_STATUS)CERT_E_UNTRUSTEDROOT || st == (SECURITY_STATUS)CERT_E_CN_NO_MATCH)
          return failCert("TLS server certificate rejected (" + hrString(st) + ")");
        return fail("TLS handshake failed (" + hrString(st) + ")");
      }
      if (!ctxOn_) return fail("TLS handshake failed");
      // need more server bytes
      uint8_t tmp[16384]; int n;
      do { n = rawRecv(tmp, sizeof tmp, end); } while (n == 0 && Clock::now() < end && !closed_);
      if (closed_) return fail("cancelled");
      if (n <= 0) return fail(n == 0 ? "TLS handshake timed out" : (err_.empty() ? "connection closed during the TLS handshake" : err_));
      enc_.insert(enc_.end(), tmp, tmp + n);
    }
    if (A.t->QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK) return fail("TLS stream sizes unavailable");
    return verifyServer(whost);
  }

  // Explicit server validation: chain building + CERT_CHAIN_POLICY_SSL with the expected host name. There is no override.
  Status verifyServer(const std::wstring& whost) {
    const SecApi& A = secApi(); PCCERT_CONTEXT cert = nullptr;
    if (A.t->QueryContextAttributesW(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK || !cert) return failCert("the server presented no certificate");
    static const char* usages[] = {szOID_PKIX_KP_SERVER_AUTH};
    CERT_CHAIN_PARA cp; ZeroMemory(&cp, sizeof cp); cp.cbSize = sizeof cp; cp.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
    cp.RequestedUsage.Usage.cUsageIdentifier = 1; cp.RequestedUsage.Usage.rgpszUsageIdentifier = const_cast<LPSTR*>(usages);
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    BOOL okc = A.getChain(nullptr, cert, nullptr, cert->hCertStore, &cp, 0, nullptr, &chain);
    if (!okc || !chain) { DWORD e = GetLastError(); A.freeCert(cert); return failCert("server certificate chain could not be built: " + certErrorText(e)); }
    std::wstring name = whost;
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl; ZeroMemory(&ssl, sizeof ssl); ssl.cbSize = sizeof ssl; ssl.dwAuthType = AUTHTYPE_SERVER; ssl.pwszServerName = &name[0];
    CERT_CHAIN_POLICY_PARA pp; ZeroMemory(&pp, sizeof pp); pp.cbSize = sizeof pp; pp.pvExtraPolicyPara = &ssl;
    CERT_CHAIN_POLICY_STATUS ps; ZeroMemory(&ps, sizeof ps); ps.cbSize = sizeof ps;
    BOOL okp = A.verifyPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps);
    DWORD perr = ps.dwError; A.freeChain(chain); A.freeCert(cert);
    if (!okp) return failCert("server certificate policy check could not run");
    if (perr != 0) return failCert("server certificate rejected: " + certErrorText(perr));
    return Status::Ok();
  }

  WSADATA wsa_; bool wsOn_ = false; SOCKET sock_ = INVALID_SOCKET; std::atomic<bool> closed_{true};
  bool tls_ = false, certFail_ = false, credOn_ = false, ctxOn_ = false; std::string err_;
  CredHandle cred_; CtxtHandle ctx_; SecPkgContext_StreamSizes sizes_;
  std::vector<uint8_t> enc_, plain_, out_;
};
} // namespace

std::unique_ptr<IByteStream> makeTcpStream() { return std::unique_ptr<IByteStream>(new TcpStream()); }
bool schannelTlsAvailable() { return secApi().ok(); }

}} // namespace
