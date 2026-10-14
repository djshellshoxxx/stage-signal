// Internal helpers shared by the Windows adapters. Not a public header.
#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include "core/types.h"
#include "platform.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>

namespace ss { namespace win { namespace detail {

// GetProcAddress with a typed result (goes through void* so -Wcast-function-type stays quiet).
template <class F> inline F procAddr(HMODULE m, const char* name) {
  return m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);
std::string narrow(const wchar_t* w, size_t n);
std::string hex(uint64_t v, int digits = 16);
uint64_t fnv1a64(const void* p, size_t n, uint64_t seed = 1469598103934665603ULL);
Ns qpcNowNs();                                   // QueryPerformanceCounter based, same epoch as std::steady_clock on mingw
Ns qpc100nsToNs(uint64_t qpcUnits100ns);         // WASAPI QPC position (100 ns units) -> Ns
std::string hrString(long hr);                   // "0x80070005"
std::string trimmed(const std::string& s);

// Initialises COM (MTA) on the calling thread once; uninitialises at thread exit. Safe if the thread is already STA.
void comThreadInit();

// Minimal COM smart pointer.
template <class T> class ComPtr {
 public:
  ComPtr() = default;
  ComPtr(const ComPtr&) = delete; ComPtr& operator=(const ComPtr&) = delete;
  ~ComPtr() { reset(); }
  T* operator->() const { return p_; }
  T* get() const { return p_; }
  T** put() { reset(); return &p_; }
  void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
  explicit operator bool() const { return p_ != nullptr; }
 private:
  T* p_ = nullptr;
};

// Pool of AudioBlocks recycled through the shared_ptr deleter (no allocation in steady state).
class AudioBlockPool : public std::enable_shared_from_this<AudioBlockPool> {
 public:
  // Returns a writable block (valid until published) and the const handle to publish. samples are resized to n.
  AudioBlockPtr acquire(AudioBlock*& writable, size_t nSamples);
  ~AudioBlockPool();
 private:
  void giveBack(AudioBlock* b);
  struct Deleter { std::shared_ptr<AudioBlockPool> pool; void operator()(const AudioBlock* b) const; };
  std::mutex m_; std::vector<AudioBlock*> free_;
};

// Lazily bound Media Foundation entry points (so the binary loads on XP).
struct MfApi {
  HMODULE plat = nullptr, rw = nullptr, mf = nullptr;
  HRESULT (WINAPI *MFStartup)(ULONG, DWORD) = nullptr;
  HRESULT (WINAPI *MFShutdown)() = nullptr;
  void*   fnCreateAttributes = nullptr;
  void*   fnCreateMediaType = nullptr;
  void*   fnEnumDeviceSources = nullptr;
  void*   fnCreateReaderFromMediaSource = nullptr;
  void*   fnCreateReaderFromURL = nullptr;
  bool ok() const { return MFStartup && fnCreateAttributes && fnCreateMediaType && fnCreateReaderFromMediaSource && fnCreateReaderFromURL; }
};
const MfApi& mfApi();                            // loads once; ok() false when MF is not installed
// RAII MFStartup/MFShutdown for one owner.
class MfSession { public: MfSession(); ~MfSession(); bool ok() const { return ok_; } private: bool ok_ = false; };

}}} // namespace
