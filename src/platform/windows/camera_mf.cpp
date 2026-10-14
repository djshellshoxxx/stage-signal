// Media Foundation camera (REQ-S09, REQ-S11, MOD-08). Camera thread owns device open/teardown; frames arrive on the MF work queue.
// Never auto-selects an alternate camera: open(id) either opens that symbolic link or fails.
#include "mf_common.h"
#include "pixconv.h"
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <thread>

namespace ss { namespace win {
using namespace detail;
namespace {

enum class Fmt { Other, YUY2, NV12, RGB24, RGB32 };
Fmt fmtOf(const GUID& g) {
  if (isGuid(g, MFVideoFormat_YUY2)) return Fmt::YUY2;
  if (isGuid(g, MFVideoFormat_NV12)) return Fmt::NV12;
  if (isGuid(g, MFVideoFormat_RGB24)) return Fmt::RGB24;
  if (isGuid(g, MFVideoFormat_RGB32) || isGuid(g, MFVideoFormat_ARGB32)) return Fmt::RGB32;
  return Fmt::Other;
}

struct Core {
  std::mutex m;                       // status + mailbox
  CameraStatus st;
  std::shared_ptr<std::function<void(const CameraStatus&)>> statusCb;
  Frame slot[2]; int pend = -1;
  uint32_t generation = 0;
  std::atomic<Ns> lastFrameNs{0};
  std::atomic<bool> closing{false};
  std::atomic<long> lostHr{0};       // nonzero: device error observed by callback
  std::string lostWhy;
  // reader (guarded by rm so ReadSample never races with teardown)
  std::mutex rm; IMFSourceReader* reader = nullptr;
  HANDLE flushEv = nullptr;
  // negotiated format (callback thread only after start)
  Fmt fmt = Fmt::Other; int w = 0, h = 0; int stride = 0; bool bottomUp = false;

  void setStatus(CameraState s, const std::string& detail) {
    CameraStatus copy; std::shared_ptr<std::function<void(const CameraStatus&)>> cb;
    { std::lock_guard<std::mutex> l(m); if (st.state == s && st.detail == detail) return; st.state = s; st.detail = detail; copy = st; cb = statusCb; }
    if (cb) (*cb)(copy);
  }
};

void refreshFormat(Core& c, IMFMediaType* t) {
  UINT32 w = 0, h = 0; mtSize(t, w, h);
  GUID sub = GUID_NULL; t->GetGUID(MF_MT_SUBTYPE, &sub);
  c.fmt = fmtOf(sub); c.w = (int)w; c.h = (int)h;
  UINT32 s = 0;
  INT32 ss_ = 0;
  if (SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) ss_ = (INT32)s;
  if (ss_ == 0) ss_ = c.fmt == Fmt::YUY2 ? (INT32)(w * 2) : c.fmt == Fmt::RGB24 ? (INT32)(w * 3) : c.fmt == Fmt::RGB32 ? (INT32)(w * 4) : (INT32)w;
  c.bottomUp = ss_ < 0 && (c.fmt == Fmt::RGB24 || c.fmt == Fmt::RGB32);
  c.stride = ss_ < 0 ? -ss_ : ss_;
}

class Callback : public IMFSourceReaderCallback {
 public:
  virtual ~Callback() = default;
  explicit Callback(std::shared_ptr<Core> c) : c_(std::move(c)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
    if (r == IID_IUnknown || r == IID_IMFSourceReaderCallback) { *o = static_cast<IMFSourceReaderCallback*>(this); AddRef(); return S_OK; }
    *o = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return (ULONG)r; }
  HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnFlush(DWORD) override { if (c_->flushEv) SetEvent(c_->flushEv); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG ts, IMFSample* sample) override {
    Core& c = *c_;
    if (c.closing) return S_OK;
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) { fail(FAILED(hr) ? hr : E_FAIL, hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED ? "camera unplugged or invalidated" : "camera read error"); return S_OK; }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { fail(E_FAIL, "camera stream ended"); return S_OK; }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      std::lock_guard<std::mutex> l(c.rm);
      IMFMediaType* t = nullptr;
      if (c.reader && SUCCEEDED(c.reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &t))) { refreshFormat(c, t); t->Release(); }
    }
    if (sample) deliver(sample, ts);
    std::lock_guard<std::mutex> l(c.rm);
    if (!c.closing && c.reader) {
      HRESULT r = c.reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
      if (FAILED(r)) fail(r, "camera read request failed");
    }
    return S_OK;
  }
 private:
  void fail(HRESULT hr, const char* why) {
    Core& c = *c_;
    if (c.lostHr.load() == 0) { c.lostWhy = std::string(why) + " (" + hrString(hr) + ")"; c.lostHr = (long)(hr ? hr : E_FAIL); }
  }
  void deliver(IMFSample* s, LONGLONG ts) {
    Core& c = *c_;
    if (c.w <= 0 || c.h <= 0 || c.fmt == Fmt::Other || c.lostHr.load() != 0) return;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(s->ConvertToContiguousBuffer(buf.put()))) return;
    BYTE* p = nullptr; DWORD mx = 0, cur = 0;
    if (FAILED(buf->Lock(&p, &mx, &cur))) return;
    int wi;
    { std::lock_guard<std::mutex> l(c.m); wi = (c.pend == 0) ? 1 : 0; }
    Frame& f = c.slot[wi];
    f.w = c.w; f.h = c.h; f.px.resize((size_t)c.w * c.h);
    bool ok = true;
    size_t need;
    switch (c.fmt) {
      case Fmt::YUY2: need = (size_t)c.stride * c.h; if ((size_t)cur >= need) pix::yuy2ToRgb32(p, c.stride, c.w, c.h, false, f.px.data()); else ok = false; break;
      case Fmt::NV12: need = (size_t)c.stride * c.h * 3 / 2; if ((size_t)cur >= need) pix::nv12ToRgb32(p, c.stride, p + (size_t)c.stride * c.h, c.stride, c.w, c.h, f.px.data()); else ok = false; break;
      case Fmt::RGB24: need = (size_t)c.stride * c.h; if ((size_t)cur >= need) pix::bgr24ToRgb32(p, c.stride, c.w, c.h, c.bottomUp, f.px.data()); else ok = false; break;
      case Fmt::RGB32: need = (size_t)c.stride * c.h; if ((size_t)cur >= need) pix::bgrx32ToRgb32(p, c.stride, c.w, c.h, c.bottomUp, f.px.data()); else ok = false; break;
      default: ok = false;
    }
    buf->Unlock();
    if (!ok) return;
    f.pts = (Ns)ts * 100; f.generation = c.generation;
    c.lastFrameNs = qpcNowNs();
    CameraState prev;
    { std::lock_guard<std::mutex> l(c.m); c.pend = wi; ++c.st.frames; prev = c.st.state; }
    if (prev != CameraState::Live) c.setStatus(CameraState::Live, "");
  }
  LONG ref_ = 1; std::shared_ptr<Core> c_;
};

// MFEnumDeviceSources wrapper; caller releases the activates and frees the array.
struct DeviceList {
  IMFActivate** a = nullptr; UINT32 n = 0;
  ~DeviceList() { for (UINT32 i = 0; i < n; ++i) if (a[i]) a[i]->Release(); if (a) CoTaskMemFree(a); }
};
HRESULT enumDevices(DeviceList& out) {
  const MfApi& api = mfApi();
  if (!api.ok() || !api.fnEnumDeviceSources) return E_NOTIMPL;
  ComPtr<IMFAttributes> at;
  HRESULT hr = ((PFN_MFCreateAttributes)api.fnCreateAttributes)(at.put(), 1);
  if (FAILED(hr)) return hr;
  at->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  return ((PFN_MFEnumDeviceSources)api.fnEnumDeviceSources)(at.get(), &out.a, &out.n);
}
std::string actString(IMFActivate* a, const GUID& key) {
  WCHAR* s = nullptr; UINT32 len = 0; std::string r;
  if (SUCCEEDED(a->GetAllocatedString(key, &s, &len)) && s) { r = narrow(s, len); CoTaskMemFree(s); }
  return r;
}

class MfCamera : public ICamera {
 public:
  MfCamera() : core_(std::make_shared<Core>()) {}
  ~MfCamera() override { close(); }

  std::vector<CameraInfo> enumerate() override {
    std::vector<CameraInfo> out;
    comThreadInit();
    MfSession ms; if (!ms.ok()) return out;
    DeviceList dl;
    if (FAILED(enumDevices(dl))) return out;
    for (UINT32 i = 0; i < dl.n; ++i) {
      CameraInfo ci; ci.id = actString(dl.a[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
      ci.name = actString(dl.a[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
      if (!ci.id.empty()) out.push_back(ci);
    }
    return out;
  }

  Status open(const std::string& id, CameraFormat want) override {
    std::lock_guard<std::mutex> ol(openMtx_);
    closeLocked();
    if (id.empty()) return Status::Fail(Err::InvalidArgument, "camera id required (no automatic selection)");
    if (!mediaFoundationAvailable()) return Status::Fail(Err::UnsupportedCapability, "Media Foundation is not available on this Windows version");
    core_ = std::make_shared<Core>();           // fresh state; stale callbacks of the previous session keep the old Core
    {
      std::lock_guard<std::mutex> l(statusM_);
      core_->statusCb = cb_;
    }
    ++gen_;
    core_->generation = gen_;
    core_->st = CameraStatus(); core_->st.state = CameraState::Opening;
    want_ = want; id_ = id;
    initDone_ = false; initRes_ = Status::Ok();
    stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::shared_ptr<Core> core = core_;
    th_ = std::thread([this, core] { run(core); });
    std::unique_lock<std::mutex> l(im_);
    if (!icv_.wait_for(l, std::chrono::seconds(15), [this] { return initDone_; })) {
      l.unlock(); closeLocked();
      return Status::Fail(Err::DeviceLost, "camera open timed out");
    }
    Status r = initRes_; l.unlock();
    if (!r.ok()) closeLocked();
    return r;
  }

  void close() override { std::lock_guard<std::mutex> ol(openMtx_); closeLocked(); std::lock_guard<std::mutex> l(core_->m); core_->st.state = CameraState::Closed; core_->st.detail.clear(); }

  CameraStatus status() const override { std::lock_guard<std::mutex> l(core_->m); return core_->st; }

  bool latest(Frame& out) override {
    std::shared_ptr<Core> c = core_;
    std::lock_guard<std::mutex> l(c->m);
    if (c->pend < 0) return false;
    Frame& s = c->slot[c->pend];
    out.w = s.w; out.h = s.h; out.pts = s.pts; out.generation = s.generation;
    out.px.swap(s.px);            // zero-copy hand-over; slot receives caller's old buffer for reuse
    c->pend = -1;
    return true;
  }

  void setStatusCallback(std::function<void(const CameraStatus&)> cb) override {
    auto p = cb ? std::make_shared<std::function<void(const CameraStatus&)>>(std::move(cb)) : nullptr;
    { std::lock_guard<std::mutex> l(statusM_); cb_ = p; }
    std::lock_guard<std::mutex> l(core_->m); core_->statusCb = p;
  }

 private:
  void closeLocked() {
    if (th_.joinable()) { SetEvent(stopEv_); th_.join(); }
    if (stopEv_) { CloseHandle(stopEv_); stopEv_ = nullptr; }
  }
  void finish(const Status& s) { { std::lock_guard<std::mutex> l(im_); initRes_ = s; initDone_ = true; } icv_.notify_all(); }

  void run(std::shared_ptr<Core> core) {
    Core& c = *core;
    comThreadInit();
    MfSession ms;
    auto bail = [&](CameraState st, Err e, const std::string& d) { c.setStatus(st, d); finish(Status::Fail(e, d)); };
    if (!ms.ok()) return bail(CameraState::Error, Err::UnsupportedCapability, "Media Foundation failed to start");
    const MfApi& api = mfApi();

    ComPtr<IMFMediaSource> src;
    {
      DeviceList dl; HRESULT hr = enumDevices(dl);
      if (FAILED(hr)) return bail(CameraState::Error, Err::DeviceLost, "camera enumeration failed (" + hrString(hr) + ")");
      for (UINT32 i = 0; i < dl.n && !src; ++i)
        if (actString(dl.a[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK) == id_) {
          hr = dl.a[i]->ActivateObject(IID_IMFMediaSource, (void**)src.put());
          if (FAILED(hr)) return bail(hr == E_ACCESSDENIED ? CameraState::Error : CameraState::Lost, hr == E_ACCESSDENIED ? Err::PermissionDenied : Err::DeviceLost,
                                      hr == E_ACCESSDENIED ? "camera access denied by the system" : "camera could not be activated (" + hrString(hr) + ")");
        }
      if (!src) return bail(CameraState::Lost, Err::DeviceLost, "selected camera is not present; no alternate was chosen");
    }
    c.flushEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Callback* cbObj = new Callback(core);
    ComPtr<IMFAttributes> at;
    ((PFN_MFCreateAttributes)api.fnCreateAttributes)(at.put(), 3);
    at->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, cbObj);
    at->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   // honoured on Win7+: lets the reader decode MJPG / convert
    ComPtr<IMFSourceReader> rd;
    HRESULT hr = ((PFN_MFCreateSourceReaderFromMediaSource)api.fnCreateReaderFromMediaSource)(src.get(), at.get(), rd.put());
    cbObj->Release();
    auto cleanup = [&]() {
      { std::lock_guard<std::mutex> l(c.rm); c.closing = true; }
      if (rd) {
        rd->Flush((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        WaitForSingleObject(c.flushEv, 1000);
        std::lock_guard<std::mutex> l(c.rm); c.reader = nullptr;
      }
      rd.reset();
      if (src) src->Shutdown();
      src.reset();
      CloseHandle(c.flushEv); c.flushEv = nullptr;
    };
    if (FAILED(hr)) { cleanup(); return bail(CameraState::Error, Err::DeviceLost, "camera reader failed (" + hrString(hr) + ")"); }

    // ---- format negotiation: pick closest native mode to want.w x want.h @ want.fps
    struct Cand { DWORD idx; double score; };
    std::vector<Cand> cands;
    for (DWORD i = 0; i < 256; ++i) {
      ComPtr<IMFMediaType> t;
      if (FAILED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, t.put()))) break;
      UINT32 w = 0, h = 0; if (!mtSize(t.get(), w, h)) continue;
      GUID sub = GUID_NULL; t->GetGUID(MF_MT_SUBTYPE, &sub);
      double fps = mtFps(t.get());
      double sc = std::fabs((double)w - want_.w) + std::fabs((double)h - want_.h);
      sc = sc * 100.0 + std::fabs(fps - want_.fps) * 10.0 + (fmtOf(sub) == Fmt::Other ? 5.0 : 0.0);
      cands.push_back({i, sc});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
    bool set = false;
    for (size_t k = 0; k < cands.size() && k < 12 && !set; ++k) {
      ComPtr<IMFMediaType> t;
      if (FAILED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, cands[k].idx, t.put()))) continue;
      GUID sub = GUID_NULL; t->GetGUID(MF_MT_SUBTYPE, &sub);
      if (fmtOf(sub) != Fmt::Other && SUCCEEDED(rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.get()))) { set = true; break; }
      // compressed (e.g. MJPG): ask the reader for RGB32 at the same size/rate
      ComPtr<IMFMediaType> o;
      if (SUCCEEDED(((PFN_MFCreateMediaType)api.fnCreateMediaType)(o.put()))) {
        UINT64 sz = 0, fr = 0; t->GetUINT64(MF_MT_FRAME_SIZE, &sz); t->GetUINT64(MF_MT_FRAME_RATE, &fr);
        o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); o->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        o->SetUINT64(MF_MT_FRAME_SIZE, sz); if (fr) o->SetUINT64(MF_MT_FRAME_RATE, fr);
        if (SUCCEEDED(rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, o.get()))) { set = true; break; }
      }
    }
    if (!set) { cleanup(); return bail(CameraState::Error, Err::UnsupportedCapability, "no usable camera format (needs YUY2/NV12/RGB or a system MJPG decoder)"); }
    {
      ComPtr<IMFMediaType> cur;
      if (FAILED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, cur.put()))) { cleanup(); return bail(CameraState::Error, Err::DeviceLost, "camera format query failed"); }
      refreshFormat(c, cur.get());
      double fps = mtFps(cur.get());
      std::lock_guard<std::mutex> l(c.m);
      c.st.negotiated.w = c.w; c.st.negotiated.h = c.h; c.st.negotiated.fps = (int)(fps + 0.5);
    }
    if (c.fmt == Fmt::Other) { cleanup(); return bail(CameraState::Error, Err::UnsupportedCapability, "camera delivers an unsupported pixel format"); }
    c.lastFrameNs = qpcNowNs();
    { std::lock_guard<std::mutex> l(c.rm); c.reader = rd.get(); }
    hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) { cleanup(); return bail(hr == E_ACCESSDENIED ? CameraState::Error : CameraState::Lost, hr == E_ACCESSDENIED ? Err::PermissionDenied : Err::DeviceLost, "camera start failed (" + hrString(hr) + ")"); }
    finish(Status::Ok());

    // ---- watchdog: Live -> Frozen (stall) -> Lost (<= 500 ms of freeze)
    Ns openedNs = qpcNowNs();
    const int fps = std::max(1, c.st.negotiated.fps);
    const Ns freezeAfter = std::max<Ns>(100 * kNsPerMs, (Ns)(2.5 * kNsPerSec / fps));
    bool lost = false;
    while (WaitForSingleObject(stopEv_, 20) == WAIT_TIMEOUT) {
      Ns now = qpcNowNs();
      if (c.lostHr.load() != 0) { c.setStatus(CameraState::Lost, c.lostWhy); lost = true; break; }
      CameraState st; { std::lock_guard<std::mutex> l(c.m); st = c.st.state; }
      if (st == CameraState::Opening) { if (now - openedNs > 8 * kNsPerSec) { c.setStatus(CameraState::Error, "camera produced no frames"); lost = true; break; } continue; }
      Ns gap = now - c.lastFrameNs.load();
      if (gap > freezeAfter + 500 * kNsPerMs) { c.setStatus(CameraState::Lost, "camera stopped delivering frames"); }
      else if (gap > freezeAfter) { if (st == CameraState::Live) c.setStatus(CameraState::Frozen, "no new frames"); }
    }
    (void)lost;
    cleanup();
  }

  std::shared_ptr<Core> core_;
  std::mutex openMtx_, statusM_, im_; std::condition_variable icv_; bool initDone_ = false; Status initRes_;
  std::shared_ptr<std::function<void(const CameraStatus&)>> cb_;
  CameraFormat want_; std::string id_; uint32_t gen_ = 0;
  std::thread th_; HANDLE stopEv_ = nullptr;
};

} // namespace

bool mediaFoundationAvailable() { return mfApi().ok(); }
std::unique_ptr<ICamera> makeCamera() { return std::unique_ptr<ICamera>(new MfCamera()); }

}} // namespace
