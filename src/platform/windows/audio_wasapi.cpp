// WASAPI shared-mode, event-driven capture (REQ-A01/A02, IF-01). COM lives on the capture thread.
#include "win_internal.h"
#include "audio_backends.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <thread>

namespace ss { namespace win {
using namespace detail;
namespace {

const PROPERTYKEY kPkeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const DWORD kFlagAutoConvertPcm = 0x80000000u, kFlagSrcDefaultQuality = 0x08000000u;

std::string deviceFriendlyName(IMMDevice* dev) {
  ComPtr<IPropertyStore> ps;
  if (FAILED(dev->OpenPropertyStore(STGM_READ, ps.put()))) return std::string();
  PROPVARIANT pv; PropVariantInit(&pv);
  std::string out;
  if (SUCCEEDED(ps->GetValue(kPkeyFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) out = narrow(pv.pwszVal, wcslen(pv.pwszVal));
  PropVariantClear(&pv);
  return out;
}
std::wstring deviceId(IMMDevice* dev) {
  LPWSTR id = nullptr; std::wstring s;
  if (SUCCEEDED(dev->GetId(&id)) && id) { s = id; CoTaskMemFree(id); }
  return s;
}

// Watches for removal/disable of OUR endpoint only; default-device changes are ignored on purpose (never auto-switch).
class Notify : public IMMNotificationClient {
 public:
  virtual ~Notify() = default;
  Notify(std::wstring id, HANDLE ev) : id_(std::move(id)), ev_(ev) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
    if (r == IID_IUnknown || r == IID_IMMNotificationClient) { *o = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK; }
    *o = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return (ULONG)r; }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR d, DWORD st) override { if (d && id_ == d && st != DEVICE_STATE_ACTIVE) SetEvent(ev_); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR d) override { if (d && id_ == d) SetEvent(ev_); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
 private:
  LONG ref_ = 1; std::wstring id_; HANDLE ev_;
};

WAVEFORMATEXTENSIBLE floatFormat(uint32_t rate, WORD ch) {
  WAVEFORMATEXTENSIBLE f; ZeroMemory(&f, sizeof f);
  f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  f.Format.nChannels = ch; f.Format.nSamplesPerSec = rate; f.Format.wBitsPerSample = 32;
  f.Format.nBlockAlign = (WORD)(ch * 4); f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
  f.Format.cbSize = 22; f.Samples.wValidBitsPerSample = 32;
  f.dwChannelMask = ch == 1 ? 0x4 : 0x3;
  f.SubFormat = kSubtypeFloat;
  return f;
}

class WasapiCapture : public IAudioCapture {
 public:
  WasapiCapture() : pool_(std::make_shared<AudioBlockPool>()) {}
  ~WasapiCapture() override { close(); }

  std::vector<AudioDeviceInfo> enumerate() override {
    std::vector<AudioDeviceInfo> out;
    comThreadInit();
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)en.put()))) return out;
    std::wstring defId;
    { ComPtr<IMMDevice> d; if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, d.put()))) defId = deviceId(d.get()); }
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, col.put()))) return out;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
      ComPtr<IMMDevice> d;
      if (FAILED(col->Item(i, d.put()))) continue;
      AudioDeviceInfo info;
      std::wstring id = deviceId(d.get());
      info.id = narrow(id); info.name = deviceFriendlyName(d.get()); info.isDefault = (id == defId);
      ComPtr<IAudioClient> ac;
      if (SUCCEEDED(d->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)ac.put()))) {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(ac->GetMixFormat(&mix)) && mix) { info.channels = mix->nChannels; info.rate = mix->nSamplesPerSec; CoTaskMemFree(mix); }
      }
      out.push_back(std::move(info));
    }
    return out;
  }

  Status open(const AudioOpenParams& p) override {
    std::lock_guard<std::mutex> ol(openMtx_);
    closeLocked();
    {
      std::lock_guard<std::mutex> l(m_);
      ++generation_;
      status_ = AudioStatus(); status_.generation = generation_; status_.state = AudioState::Opening;
      status_.deviceName = std::string();
    }
    params_ = p;
    initDone_ = false; initResult_ = Status::Ok();
    stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
    std::unique_lock<std::mutex> l(initM_);
    if (!initCv_.wait_for(l, std::chrono::seconds(10), [this] { return initDone_; })) {
      l.unlock();
      closeLocked();
      setState(AudioState::Unavailable, "open timed out");
      return Status::Fail(Err::DeviceLost, "WASAPI open timed out");
    }
    Status r = initResult_;
    l.unlock();
    if (!r.ok()) closeLocked();                   // thread already ended; join it. State stays Unavailable/Denied.
    return r;
  }

  void close() override { std::lock_guard<std::mutex> ol(openMtx_); closeLocked(); { std::lock_guard<std::mutex> l(m_); if (status_.state != AudioState::Closed) { status_.state = AudioState::Closed; status_.detail.clear(); } } }

  AudioStatus status() const override { std::lock_guard<std::mutex> l(m_); return status_; }
  void setBlockCallback(std::function<void(AudioBlockPtr)> cb) override {
    std::lock_guard<std::mutex> l(m_); blockCb_ = cb ? std::make_shared<std::function<void(AudioBlockPtr)>>(std::move(cb)) : nullptr;
  }
  void setStatusCallback(std::function<void(const AudioStatus&)> cb) override {
    std::lock_guard<std::mutex> l(m_); statusCb_ = cb ? std::make_shared<std::function<void(const AudioStatus&)>>(std::move(cb)) : nullptr;
  }

 private:
  void closeLocked() {
    if (thread_.joinable()) { if (stopEv_) SetEvent(stopEv_); thread_.join(); }
    if (stopEv_) { CloseHandle(stopEv_); stopEv_ = nullptr; }
  }
  void setState(AudioState s, const std::string& detail) {
    AudioStatus copy; std::shared_ptr<std::function<void(const AudioStatus&)>> cb;
    { std::lock_guard<std::mutex> l(m_); status_.state = s; status_.detail = detail; copy = status_; cb = statusCb_; }
    if (cb) (*cb)(copy);
  }
  void finishInit(const Status& st) {
    { std::lock_guard<std::mutex> l(initM_); initResult_ = st; initDone_ = true; }
    initCv_.notify_all();
  }
  void failInit(AudioState s, Err e, const std::string& d) {
    setState(s, d);
    finishInit(Status::Fail(e, d));
  }
  static bool isDenied(HRESULT hr) { return hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED); }

  void run() {
    comThreadInit();
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)en.put());
    if (FAILED(hr)) return failInit(AudioState::Unavailable, Err::UnsupportedCapability, "WASAPI not available on this system (" + hrString(hr) + ")");
    ComPtr<IMMDevice> dev;
    // An empty id means "the endpoint that is default right now" and is an explicit operator choice; a given id is never replaced.
    if (params_.deviceId.empty()) hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, dev.put());
    else hr = en->GetDevice(widen(params_.deviceId).c_str(), dev.put());
    if (FAILED(hr)) return failInit(isDenied(hr) ? AudioState::Denied : AudioState::Unavailable, isDenied(hr) ? Err::PermissionDenied : Err::DeviceLost,
                                    "Selected audio device not found (" + hrString(hr) + ")");
    DWORD st = 0;
    if (SUCCEEDED(dev->GetState(&st)) && st != DEVICE_STATE_ACTIVE)
      return failInit(AudioState::Unavailable, Err::DeviceLost, "Selected audio device is disabled, unplugged or not present");
    std::wstring wid = deviceId(dev.get());
    std::string name = deviceFriendlyName(dev.get());
    { std::lock_guard<std::mutex> l(m_); status_.deviceName = name; }

    HANDLE bufEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE goneEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Notify* nc = new Notify(wid, goneEv);
    en->RegisterEndpointNotificationCallback(nc);

    ComPtr<IAudioClient> ac; ComPtr<IAudioCaptureClient> cap;
    uint32_t rate = 0; HRESULT lastHr = S_OK; bool inited = false;
    // Candidate rates: requested, the other baseline rate, then the engine mix rate (0 = filled in below).
    uint32_t cands[3] = {params_.rate, 0, 0};
    if (params_.rate == 44100) cands[1] = 48000; else if (params_.rate == 48000) cands[1] = 44100;
    for (int i = 0; i < 3 && !inited; ++i) {
      ac.reset();
      hr = dev->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)ac.put());
      if (FAILED(hr)) { lastHr = hr; if (isDenied(hr)) break; continue; }
      uint32_t r = cands[i];
      if (r == 0 && (i == 2 || params_.rate == 0)) {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(ac->GetMixFormat(&mix)) && mix) { r = mix->nSamplesPerSec; CoTaskMemFree(mix); }
      }
      if (r == 0) continue;
      WAVEFORMATEXTENSIBLE f = floatFormat(r, 2);
      hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | kFlagAutoConvertPcm | kFlagSrcDefaultQuality,
                          1000000 /*100 ms*/, 0, &f.Format, nullptr);
      if (SUCCEEDED(hr)) { rate = r; inited = true; } else { lastHr = hr; if (isDenied(hr)) break; }
    }
    auto teardownEarly = [&](AudioState s, Err e, const std::string& d) {
      en->UnregisterEndpointNotificationCallback(nc); nc->Release(); CloseHandle(bufEv); CloseHandle(goneEv);
      failInit(s, e, d);
    };
    if (!inited) {
      if (isDenied(lastHr)) return teardownEarly(AudioState::Denied, Err::PermissionDenied, "Microphone access denied by the system (" + hrString(lastHr) + ")");
      if (lastHr == AUDCLNT_E_DEVICE_IN_USE) return teardownEarly(AudioState::Unavailable, Err::DeviceLost, "Device is in use exclusively by another application");
      if (lastHr == AUDCLNT_E_UNSUPPORTED_FORMAT) return teardownEarly(AudioState::Unavailable, Err::UnsupportedCapability, "Device rejected 44.1/48 kHz float capture");
      return teardownEarly(AudioState::Unavailable, Err::DeviceLost, "Could not open audio device (" + hrString(lastHr) + ")");
    }
    hr = ac->SetEventHandle(bufEv);
    if (SUCCEEDED(hr)) hr = ac->GetService(IID_IAudioCaptureClient, (void**)cap.put());
    if (SUCCEEDED(hr)) hr = ac->Start();
    if (FAILED(hr)) { cap.reset(); ac.reset(); return teardownEarly(isDenied(hr) ? AudioState::Denied : AudioState::Unavailable, isDenied(hr) ? Err::PermissionDenied : Err::DeviceLost, "Could not start capture (" + hrString(hr) + ")"); }

    const uint32_t outCh = params_.channelRoute == 0 ? 2u : 1u;
    {
      std::lock_guard<std::mutex> l(m_);
      status_.rate = rate; status_.channels = outCh;
    }
    setState(AudioState::Running, "");
    finishInit(Status::Ok());

    uint32_t gen; { std::lock_guard<std::mutex> l(m_); gen = generation_; }
    uint64_t seq = 0, nextSample = 0, basePos = 0; bool haveBase = false;
    bool disconnected = false; std::string why;
    HANDLE waits[3] = {stopEv_, bufEv, goneEv};
    while (!disconnected) {
      DWORD w = WaitForMultipleObjects(3, waits, FALSE, 250);
      if (w == WAIT_OBJECT_0) break;
      if (w == WAIT_OBJECT_0 + 2) { disconnected = true; why = "Selected audio device was removed or disabled"; break; }
      // drain packets (also executed on timeout: GetNextPacketSize doubles as the liveness probe, <= 250 ms)
      for (;;) {
        UINT32 pkt = 0;
        hr = cap->GetNextPacketSize(&pkt);
        if (FAILED(hr)) { disconnected = true; why = (hr == AUDCLNT_E_DEVICE_INVALIDATED ? "Audio device invalidated (removed, disabled or format changed)" : "Audio device error " + hrString(hr)); break; }
        if (pkt == 0) break;
        BYTE* data = nullptr; UINT32 frames = 0; DWORD fl = 0; UINT64 devPos = 0, qpcPos = 0;
        hr = cap->GetBuffer(&data, &frames, &fl, &devPos, &qpcPos);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
        if (FAILED(hr)) { disconnected = true; why = (hr == AUDCLNT_E_DEVICE_INVALIDATED ? "Audio device invalidated" : "Audio device error " + hrString(hr)); break; }
        uint32_t flags = 0;
        if (fl & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
          flags |= AF_Discontinuity;
          std::lock_guard<std::mutex> l(m_); ++status_.overruns;
        }
        if (!haveBase) { basePos = devPos; haveBase = true; }
        else if ((fl & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && devPos >= basePos && devPos - basePos > nextSample) nextSample = devPos - basePos;  // account for lost frames
        Ns capNs;
        if (!(fl & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpcPos != 0) { capNs = qpc100nsToNs(qpcPos); flags |= AF_TimestampValid; }
        else capNs = qpcNowNs() - (Ns)((int64_t)frames * kNsPerSec / (int64_t)rate);
        AudioBlock* b = nullptr;
        AudioBlockPtr bp = pool_->acquire(b, (size_t)frames * outCh);
        b->rate = rate; b->channels = outCh; b->firstSample = nextSample; b->captureNs = capNs;
        b->generation = gen; b->sequence = ++seq;
        float* o = b->samples.data();
        if (fl & AUDCLNT_BUFFERFLAGS_SILENT) {
          flags |= AF_Silent;
          for (size_t i = 0; i < (size_t)frames * outCh; ++i) o[i] = 0.0f;
        } else {
          const float* s = reinterpret_cast<const float*>(data);
          const int route = params_.channelRoute;
          for (UINT32 i = 0; i < frames; ++i) {
            float l = s[2 * i], r = s[2 * i + 1];
            if (!std::isfinite(l)) l = 0.0f;
            if (!std::isfinite(r)) r = 0.0f;
            if (route == 0) { o[2 * i] = l; o[2 * i + 1] = r; }
            else o[i] = (route == 1) ? l : r;
          }
        }
        b->flags = flags;
        nextSample += frames;
        cap->ReleaseBuffer(frames);
        std::shared_ptr<std::function<void(AudioBlockPtr)>> cb;
        { std::lock_guard<std::mutex> l(m_); cb = blockCb_; }
        if (cb) (*cb)(std::move(bp));
      }
    }
    ac->Stop();
    en->UnregisterEndpointNotificationCallback(nc);
    cap.reset(); ac.reset();
    nc->Release(); CloseHandle(bufEv); CloseHandle(goneEv);
    if (disconnected) setState(AudioState::Disconnected, why + "; no other device was selected");
  }

  std::shared_ptr<AudioBlockPool> pool_;
  std::mutex openMtx_;
  mutable std::mutex m_;
  AudioStatus status_;
  uint32_t generation_ = 0;
  std::shared_ptr<std::function<void(AudioBlockPtr)>> blockCb_;
  std::shared_ptr<std::function<void(const AudioStatus&)>> statusCb_;
  AudioOpenParams params_;
  std::thread thread_; HANDLE stopEv_ = nullptr;
  std::mutex initM_; std::condition_variable initCv_; bool initDone_ = false; Status initResult_;
};

} // namespace

std::unique_ptr<IAudioCapture> makeWasapiCapture() { return std::unique_ptr<IAudioCapture>(new WasapiCapture()); }

bool wasapiAvailable() {
  comThreadInit();
  ComPtr<IMMDeviceEnumerator> en;
  return SUCCEEDED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)en.put()));
}

}} // namespace
