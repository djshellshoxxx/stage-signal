// WinMM waveIn capture: explicit fallback backend for pre-Vista systems or when WASAPI is not chosen. (REQ-A01/A02)
// Limitations (honest): no device clock, so captureNs is host-estimated and AF_TimestampValid is never set.
#include "win_internal.h"
#include "audio_backends.h"
#include <mmsystem.h>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <thread>
#include <map>

namespace ss { namespace win {
using namespace detail;
namespace {

constexpr int kBuffers = 8;
constexpr int kBufMs = 20;

struct WinmmDev { UINT index; std::string key, name; uint32_t channels; };

std::vector<WinmmDev> listDevices() {
  std::vector<WinmmDev> out;
  UINT n = waveInGetNumDevs();
  std::map<std::string, int> seen;
  for (UINT i = 0; i < n; ++i) {
    WAVEINCAPSW c; ZeroMemory(&c, sizeof c);
    if (waveInGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
    std::string name = narrow(c.szPname, wcsnlen(c.szPname, 32));
    std::string base = "winmm:" + name + "|" + hex(c.wMid, 4) + "." + hex(c.wPid, 4);
    int k = seen[base]++;
    WinmmDev d; d.index = i; d.name = name; d.channels = c.wChannels;
    d.key = base + "#" + std::to_string(k);        // index among identical devices disambiguates twins
    out.push_back(d);
  }
  return out;
}

class WinmmCapture : public IAudioCapture {
 public:
  WinmmCapture() : pool_(std::make_shared<AudioBlockPool>()) {}
  ~WinmmCapture() override { close(); }

  std::vector<AudioDeviceInfo> enumerate() override {
    std::vector<AudioDeviceInfo> out;
    auto devs = listDevices();
    for (size_t i = 0; i < devs.size(); ++i) {
      AudioDeviceInfo d; d.id = devs[i].key; d.name = devs[i].name; d.isDefault = (i == 0); d.channels = devs[i].channels; d.rate = 0;
      out.push_back(d);
    }
    return out;
  }

  Status open(const AudioOpenParams& p) override {
    std::lock_guard<std::mutex> ol(openMtx_);
    closeLocked();
    { std::lock_guard<std::mutex> l(m_); ++generation_; status_ = AudioStatus(); status_.generation = generation_; status_.state = AudioState::Opening; }
    params_ = p;
    auto devs = listDevices();
    const WinmmDev* sel = nullptr;
    if (p.deviceId.empty()) { if (!devs.empty()) sel = &devs[0]; }
    else for (auto& d : devs) if (d.key == p.deviceId) { sel = &d; break; }
    if (!sel) { setState(AudioState::Unavailable, "Selected audio device not found"); return Status::Fail(Err::DeviceLost, "Selected audio device not found"); }
    { std::lock_guard<std::mutex> l(m_); status_.deviceName = sel->name; }
    devIndex_ = sel->index; devKey_ = sel->key;

    // Try requested rate then the other baseline rate; stereo then mono; 16-bit PCM.
    uint32_t rates[2] = {p.rate ? p.rate : 48000u, (p.rate == 44100) ? 48000u : 44100u};
    stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    doneEv_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    MMRESULT mr = MMSYSERR_ERROR;
    for (int ri = 0; ri < 2 && mr != MMSYSERR_NOERROR; ++ri)
      for (WORD ch = 2; ch >= 1 && mr != MMSYSERR_NOERROR; --ch) {
        WAVEFORMATEX f; ZeroMemory(&f, sizeof f);
        f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = ch; f.nSamplesPerSec = rates[ri]; f.wBitsPerSample = 16;
        f.nBlockAlign = (WORD)(ch * 2); f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
        mr = waveInOpen(&hwi_, devIndex_, &f, (DWORD_PTR)doneEv_, 0, CALLBACK_EVENT);
        if (mr == MMSYSERR_NOERROR) { rate_ = rates[ri]; inCh_ = ch; }
        if (mr == MMSYSERR_ALLOCATED) ri = 2;   // in use: no point trying other formats
      }
    if (mr != MMSYSERR_NOERROR) {
      CloseHandle(stopEv_); CloseHandle(doneEv_); stopEv_ = doneEv_ = nullptr; hwi_ = nullptr;
      std::string d = mr == MMSYSERR_ALLOCATED ? "Device is in use by another application" : "Device rejected 16-bit PCM capture at 44.1/48 kHz (MMRESULT " + std::to_string(mr) + ")";
      bool denied = false;
      setState(denied ? AudioState::Denied : AudioState::Unavailable, d);
      return Status::Fail(mr == MMSYSERR_ALLOCATED ? Err::DeviceLost : Err::UnsupportedCapability, d);
    }
    frameBytes_ = inCh_ * 2u;
    bufFrames_ = rate_ * kBufMs / 1000;
    for (int i = 0; i < kBuffers; ++i) {
      mem_[i].assign((size_t)bufFrames_ * frameBytes_, 0);
      ZeroMemory(&hdr_[i], sizeof(WAVEHDR));
      hdr_[i].lpData = (LPSTR)mem_[i].data(); hdr_[i].dwBufferLength = (DWORD)mem_[i].size();
      waveInPrepareHeader(hwi_, &hdr_[i], sizeof(WAVEHDR));
      waveInAddBuffer(hwi_, &hdr_[i], sizeof(WAVEHDR));
    }
    outCh_ = p.channelRoute == 0 ? 2u : 1u;
    { std::lock_guard<std::mutex> l(m_); status_.rate = rate_; status_.channels = outCh_; }
    waveInStart(hwi_);
    setState(AudioState::Running, inCh_ == 1 && outCh_ == 2 ? "mono device: channel duplicated" : "");
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return Status::Ok();
  }

  void close() override { std::lock_guard<std::mutex> ol(openMtx_); closeLocked(); std::lock_guard<std::mutex> l(m_); if (status_.state != AudioState::Closed) { status_.state = AudioState::Closed; status_.detail.clear(); } }
  AudioStatus status() const override { std::lock_guard<std::mutex> l(m_); return status_; }
  void setBlockCallback(std::function<void(AudioBlockPtr)> cb) override { std::lock_guard<std::mutex> l(m_); blockCb_ = cb ? std::make_shared<std::function<void(AudioBlockPtr)>>(std::move(cb)) : nullptr; }
  void setStatusCallback(std::function<void(const AudioStatus&)> cb) override { std::lock_guard<std::mutex> l(m_); statusCb_ = cb ? std::make_shared<std::function<void(const AudioStatus&)>>(std::move(cb)) : nullptr; }

 private:
  void setState(AudioState s, const std::string& detail) {
    AudioStatus copy; std::shared_ptr<std::function<void(const AudioStatus&)>> cb;
    { std::lock_guard<std::mutex> l(m_); status_.state = s; status_.detail = detail; copy = status_; cb = statusCb_; }
    if (cb) (*cb)(copy);
  }
  void closeLocked() {
    if (thread_.joinable()) { SetEvent(stopEv_); thread_.join(); }
    if (hwi_) {
      waveInReset(hwi_);
      for (int i = 0; i < kBuffers; ++i) waveInUnprepareHeader(hwi_, &hdr_[i], sizeof(WAVEHDR));
      waveInClose(hwi_); hwi_ = nullptr;
    }
    if (stopEv_) { CloseHandle(stopEv_); stopEv_ = nullptr; }
    if (doneEv_) { CloseHandle(doneEv_); doneEv_ = nullptr; }
  }
  bool deviceStillPresent() {
    for (auto& d : listDevices()) if (d.key == devKey_) return true;
    return false;
  }

  void run() {
    uint32_t gen; { std::lock_guard<std::mutex> l(m_); gen = generation_; }
    HANDLE waits[2] = {stopEv_, doneEv_};
    uint64_t next = 0, seq = 0;
    Ns lastDone = qpcNowNs();
    int slot = 0;
    bool lost = false; std::string why;
    while (!lost) {
      DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
      if (w == WAIT_OBJECT_0) break;
      int ready = 0;
      for (int k = 0; k < kBuffers; ++k) if (hdr_[k].dwFlags & WHDR_DONE) ++ready;
      bool backlog = ready >= kBuffers - 1;
      while (hdr_[slot].dwFlags & WHDR_DONE) {
        WAVEHDR& h = hdr_[slot];
        size_t frames = h.dwBytesRecorded / frameBytes_;
        Ns now = qpcNowNs(); lastDone = now;
        if (frames) {
          AudioBlock* b = nullptr;
          AudioBlockPtr bp = pool_->acquire(b, frames * outCh_);
          b->rate = rate_; b->channels = outCh_; b->firstSample = next; b->generation = gen; b->sequence = ++seq;
          b->captureNs = now - (Ns)((int64_t)frames * kNsPerSec / (int64_t)rate_);   // estimate: completion time minus duration
          b->flags = backlog ? (uint32_t)AF_Discontinuity : 0u;
          if (backlog) { std::lock_guard<std::mutex> l(m_); ++status_.overruns; backlog = false; }
          const int16_t* s = reinterpret_cast<const int16_t*>(h.lpData);
          float* o = b->samples.data();
          const float k = 1.0f / 32768.0f;
          for (size_t i = 0; i < frames; ++i) {
            float l = s[inCh_ * i] * k, r = inCh_ == 2 ? s[inCh_ * i + 1] * k : l;
            if (params_.channelRoute == 0) { o[2 * i] = l; o[2 * i + 1] = r; }
            else o[i] = params_.channelRoute == 1 ? l : r;
          }
          next += frames;
          std::shared_ptr<std::function<void(AudioBlockPtr)>> cb;
          { std::lock_guard<std::mutex> l(m_); cb = blockCb_; }
          if (cb) (*cb)(std::move(bp));
        }
        h.dwFlags &= ~WHDR_DONE; h.dwBytesRecorded = 0;
        MMRESULT mr = waveInAddBuffer(hwi_, &h, sizeof(WAVEHDR));
        if (mr != MMSYSERR_NOERROR) { lost = true; why = "waveIn error " + std::to_string(mr); break; }
        slot = (slot + 1) % kBuffers;
      }
      if (lost) break;
      if (qpcNowNs() - lastDone > 700 * kNsPerMs) { lost = true; why = "Audio device stopped delivering data"; }
      else if (w == WAIT_TIMEOUT && !deviceStillPresent()) { lost = true; why = "Audio device is no longer present"; }
    }
    if (lost) setState(AudioState::Disconnected, why + "; no other device was selected");
  }

  std::shared_ptr<AudioBlockPool> pool_;
  std::mutex openMtx_; mutable std::mutex m_;
  AudioStatus status_; uint32_t generation_ = 0;
  std::shared_ptr<std::function<void(AudioBlockPtr)>> blockCb_;
  std::shared_ptr<std::function<void(const AudioStatus&)>> statusCb_;
  AudioOpenParams params_;
  UINT devIndex_ = 0; std::string devKey_;
  HWAVEIN hwi_ = nullptr; HANDLE stopEv_ = nullptr, doneEv_ = nullptr;
  WAVEHDR hdr_[kBuffers]; std::vector<uint8_t> mem_[kBuffers];
  uint32_t rate_ = 0, inCh_ = 0, outCh_ = 0, frameBytes_ = 0, bufFrames_ = 0;
  std::thread thread_; bool running_ = false;
};

} // namespace
std::unique_ptr<IAudioCapture> makeWinmmCapture() { return std::unique_ptr<IAudioCapture>(new WinmmCapture()); }
}} // namespace
