#include "win_internal.h"
#include <cstdio>
#include <cstring>

namespace ss { namespace win { namespace detail {

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}
std::string narrow(const wchar_t* w, size_t len) {
  if (!w || !len) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, (int)len, &s[0], n, nullptr, nullptr);
  return s;
}
std::string narrow(const std::wstring& w) { return narrow(w.data(), w.size()); }

std::string hex(uint64_t v, int digits) {
  static const char* d = "0123456789abcdef";
  std::string s((size_t)digits, '0');
  for (int i = digits - 1; i >= 0; --i) { s[(size_t)i] = d[v & 15]; v >>= 4; }
  return s;
}
uint64_t fnv1a64(const void* p, size_t n, uint64_t h) {
  const unsigned char* b = (const unsigned char*)p;
  for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
  return h;
}
std::string hrString(long hr) { char b[16]; std::snprintf(b, sizeof b, "0x%08lX", (unsigned long)hr); return b; }
std::string trimmed(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= ' ') ++a;
  while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
  return s.substr(a, b - a);
}

Ns qpcNowNs() {
  LARGE_INTEGER c; QueryPerformanceCounter(&c);
  // integer split to avoid double precision loss on long uptimes
  LARGE_INTEGER f; QueryPerformanceFrequency(&f);
  if (f.QuadPart <= 0) return 0;
  int64_t s = c.QuadPart / f.QuadPart, r = c.QuadPart % f.QuadPart;
  return (Ns)(s * kNsPerSec + (int64_t)((double)r * 1e9 / (double)f.QuadPart));
}
Ns qpc100nsToNs(uint64_t u) { return (Ns)(u * 100ULL); }

namespace {
struct ComGuard {
  bool owned = false;
  ComGuard() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owned = SUCCEEDED(hr);                    // RPC_E_CHANGED_MODE => already STA: usable, not ours to release
  }
  ~ComGuard() { if (owned) CoUninitialize(); }
};
}
void comThreadInit() { thread_local ComGuard g; (void)g; }

// ---- AudioBlockPool -----------------------------------------------------------------------
void AudioBlockPool::Deleter::operator()(const AudioBlock* b) const { pool->giveBack(const_cast<AudioBlock*>(b)); }
AudioBlockPtr AudioBlockPool::acquire(AudioBlock*& w, size_t nSamples) {
  AudioBlock* b = nullptr;
  { std::lock_guard<std::mutex> l(m_); if (!free_.empty()) { b = free_.back(); free_.pop_back(); } }
  if (!b) b = new AudioBlock();
  b->samples.resize(nSamples);                // reuses capacity when recycled
  w = b;
  return AudioBlockPtr(b, Deleter{shared_from_this()});
}
void AudioBlockPool::giveBack(AudioBlock* b) {
  std::lock_guard<std::mutex> l(m_);
  if (free_.size() < 32) { free_.push_back(b); return; }
  delete b;
}
AudioBlockPool::~AudioBlockPool() { for (AudioBlock* b : free_) delete b; }

// ---- Media Foundation loader --------------------------------------------------------------
const MfApi& mfApi() {
  static MfApi api = [] {
    MfApi a;
    a.plat = LoadLibraryW(L"mfplat.dll");
    a.rw = LoadLibraryW(L"mfreadwrite.dll");
    a.mf = LoadLibraryW(L"mf.dll");
    if (!a.plat || !a.rw || !a.mf) return a;
    a.MFStartup = procAddr<HRESULT (WINAPI*)(ULONG, DWORD)>(a.plat, "MFStartup");
    a.MFShutdown = procAddr<HRESULT (WINAPI*)()>(a.plat, "MFShutdown");
    a.fnCreateAttributes = (void*)GetProcAddress(a.plat, "MFCreateAttributes");
    a.fnCreateMediaType = (void*)GetProcAddress(a.plat, "MFCreateMediaType");
    a.fnEnumDeviceSources = (void*)GetProcAddress(a.mf, "MFEnumDeviceSources");
    a.fnCreateReaderFromMediaSource = (void*)GetProcAddress(a.rw, "MFCreateSourceReaderFromMediaSource");
    a.fnCreateReaderFromURL = (void*)GetProcAddress(a.rw, "MFCreateSourceReaderFromURL");
    return a;
  }();
  return api;
}
MfSession::MfSession() {
  const MfApi& a = mfApi();
  if (!a.ok()) return;
  ok_ = SUCCEEDED(a.MFStartup(0x00020070 /*MF_VERSION*/, 0 /*MFSTARTUP_FULL*/));
}
MfSession::~MfSession() { if (ok_ && mfApi().MFShutdown) mfApi().MFShutdown(); }

}}} // namespace
