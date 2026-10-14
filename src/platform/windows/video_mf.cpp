// Media Foundation video decode via synchronous SourceReader (MOD-07). Used from the media worker thread.
// Output: 0x00RRGGBB frames, downscaled (never upscaled) to fit maxW x maxH. Audio streams are deselected (muted by default).
#include "mf_common.h"
#include "pixconv.h"
#include <cstring>

namespace ss { namespace win {
using namespace detail;
namespace {

enum class Fmt { Other, NV12, YUY2, RGB32 };

class MfVideoDecoder : public IVideoDecoder {
 public:
  ~MfVideoDecoder() override { close(); }

  Status open(const std::string& path, int maxW, int maxH) override {
    close();
    comThreadInit();
    if (!mediaFoundationAvailable()) return Status::Fail(Err::UnsupportedCapability, "Media Foundation is not available on this Windows version");
    ms_.reset(new MfSession());
    if (!ms_->ok()) { ms_.reset(); return Status::Fail(Err::UnsupportedCapability, "Media Foundation failed to start"); }
    const MfApi& api = mfApi();
    std::wstring wp = widen(path);
    if (GetFileAttributesW(wp.c_str()) == INVALID_FILE_ATTRIBUTES) { ms_.reset(); return Status::Fail(Err::InvalidArgument, "video file not found"); }
    ComPtr<IMFAttributes> at;
    ((PFN_MFCreateAttributes)api.fnCreateAttributes)(at.put(), 2);
    at->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);    // Win7+: colour conversion / decoder insertion
    HRESULT hr = ((PFN_MFCreateSourceReaderFromURL)api.fnCreateReaderFromURL)(wp.c_str(), at.get(), &reader_);
    if (FAILED(hr)) { ms_.reset(); return mapOpenError(hr, "cannot open media"); }
    reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    hr = reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (FAILED(hr)) { close(); return Status::Fail(Err::UnsupportedCapability, "file has no decodable video stream"); }

    // Prefer RGB32 (reader converts), else NV12 / YUY2 and convert here.
    const GUID* wants[3] = {&MFVideoFormat_RGB32, &MFVideoFormat_NV12, &MFVideoFormat_YUY2};
    bool set = false; HRESULT lastHr = E_FAIL;
    for (int i = 0; i < 3 && !set; ++i) {
      ComPtr<IMFMediaType> t;
      if (FAILED(((PFN_MFCreateMediaType)api.fnCreateMediaType)(t.put()))) continue;
      t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); t->SetGUID(MF_MT_SUBTYPE, *wants[i]);
      lastHr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.get());
      set = SUCCEEDED(lastHr);
    }
    if (!set) { close(); return mapOpenError(lastHr, "no decoder available for this video codec"); }
    ComPtr<IMFMediaType> cur;
    if (FAILED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, cur.put()))) { close(); return Status::Fail(Err::DecodeFailure, "cannot query decoded format"); }
    UINT32 w = 0, h = 0;
    if (!mtSize(cur.get(), w, h) || !w || !h || w > 8192 || h > 8192) { close(); return Status::Fail(Err::DecodeFailure, "invalid video dimensions"); }
    GUID sub = GUID_NULL; cur->GetGUID(MF_MT_SUBTYPE, &sub);
    fmt_ = isGuid(sub, MFVideoFormat_RGB32) ? Fmt::RGB32 : isGuid(sub, MFVideoFormat_NV12) ? Fmt::NV12 : isGuid(sub, MFVideoFormat_YUY2) ? Fmt::YUY2 : Fmt::Other;
    if (fmt_ == Fmt::Other) { close(); return Status::Fail(Err::UnsupportedCapability, "decoder produced an unsupported pixel format"); }
    srcW_ = (int)w; srcH_ = (int)h;
    UINT32 s = 0; INT32 stride = 0;
    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) stride = (INT32)s;
    if (stride == 0) stride = fmt_ == Fmt::RGB32 ? (INT32)(w * 4) : fmt_ == Fmt::YUY2 ? (INT32)(w * 2) : (INT32)w;
    bottomUp_ = stride < 0 && fmt_ == Fmt::RGB32; stride_ = stride < 0 ? -stride : stride;
    pix::fitSize(srcW_, srcH_, maxW, maxH, outW_, outH_);
    tmp_.clear();
    duration_ = 0;
    PROPVARIANT pv; PropVariantInit(&pv);
    if (SUCCEEDED(reader_->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv)) && pv.vt == VT_UI8)
      duration_ = (double)pv.uhVal.QuadPart / 1e7;
    PropVariantClear(&pv);
    eof_ = false;
    return Status::Ok();
  }

  bool nextFrame(Frame& out, Ns& ptsNs) override {
    comThreadInit();
    if (!reader_ || eof_) return false;
    for (int tries = 0; tries < 64; ++tries) {
      DWORD actual = 0, fl = 0; LONGLONG ts = 0; IMFSample* smp = nullptr;
      HRESULT hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &actual, &fl, &ts, &smp);
      if (FAILED(hr)) { eof_ = true; return false; }
      if (fl & MF_SOURCE_READERF_ENDOFSTREAM) { if (smp) smp->Release(); eof_ = true; return false; }
      if (fl & MF_SOURCE_READERF_ERROR) { if (smp) smp->Release(); eof_ = true; return false; }
      if (fl & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) refreshType();
      if (!smp) continue;                          // gap / stream tick
      bool ok = convert(smp, out);
      smp->Release();
      if (!ok) continue;
      out.pts = (Ns)ts * 100; ptsNs = out.pts;
      return true;
    }
    return false;
  }

  Status seekStart() override {
    if (!reader_) return Status::Fail(Err::InvalidArgument, "decoder not open");
    PROPVARIANT pv; PropVariantInit(&pv); pv.vt = VT_I8; pv.hVal.QuadPart = 0;
    HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, pv);
    if (FAILED(hr)) return Status::Fail(Err::DecodeFailure, "seek failed (" + hrString(hr) + ")");
    eof_ = false;
    return Status::Ok();
  }
  double durationSec() const override { return duration_; }
  void close() override {
    if (reader_) { reader_->Release(); reader_ = nullptr; }
    ms_.reset();
    eof_ = true; duration_ = 0;
  }

 private:
  static Status mapOpenError(HRESULT hr, const char* what) {
    if (hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr == MF_E_TOPO_CODEC_NOT_FOUND || hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_UNSUPPORTED_FORMAT ||
        hr == MF_E_NOT_FOUND || hr == MF_E_INVALIDREQUEST || hr == E_NOINTERFACE || hr == MF_E_TRANSFORM_TYPE_NOT_SET)
      return Status::Fail(Err::UnsupportedCapability, std::string(what) + " (unsupported container/codec " + hrString(hr) + ")");
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) return Status::Fail(Err::InvalidArgument, "video file not found");
    if (hr == E_ACCESSDENIED) return Status::Fail(Err::PermissionDenied, "video file not readable");
    return Status::Fail(Err::DecodeFailure, std::string(what) + " (" + hrString(hr) + ")");
  }
  void refreshType() {
    ComPtr<IMFMediaType> cur;
    if (FAILED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, cur.put()))) return;
    UINT32 w = 0, h = 0; if (!mtSize(cur.get(), w, h) || !w || !h) return;
    srcW_ = (int)w; srcH_ = (int)h;
    UINT32 s = 0; INT32 stride = 0;
    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) stride = (INT32)s;
    if (stride == 0) stride = fmt_ == Fmt::RGB32 ? (INT32)(w * 4) : fmt_ == Fmt::YUY2 ? (INT32)(w * 2) : (INT32)w;
    bottomUp_ = stride < 0 && fmt_ == Fmt::RGB32; stride_ = stride < 0 ? -stride : stride;
  }
  bool convert(IMFSample* smp, Frame& out) {
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(smp->ConvertToContiguousBuffer(buf.put()))) return false;
    BYTE* p = nullptr; DWORD mx = 0, cur = 0;
    if (FAILED(buf->Lock(&p, &mx, &cur))) return false;
    const size_t n = (size_t)srcW_ * srcH_;
    bool direct = (srcW_ == outW_ && srcH_ == outH_);
    std::vector<uint32_t>& dst = direct ? out.px : tmp_;
    dst.resize(n);
    bool ok = true;
    switch (fmt_) {
      case Fmt::RGB32: if ((size_t)cur >= (size_t)stride_ * srcH_) pix::bgrx32ToRgb32(p, stride_, srcW_, srcH_, bottomUp_, dst.data()); else ok = false; break;
      case Fmt::YUY2: if ((size_t)cur >= (size_t)stride_ * srcH_) pix::yuy2ToRgb32(p, stride_, srcW_, srcH_, false, dst.data()); else ok = false; break;
      case Fmt::NV12: if ((size_t)cur >= (size_t)stride_ * srcH_ * 3 / 2) pix::nv12ToRgb32(p, stride_, p + (size_t)stride_ * srcH_, stride_, srcW_, srcH_, dst.data()); else ok = false; break;
      default: ok = false;
    }
    buf->Unlock();
    if (!ok) return false;
    if (direct) { out.w = srcW_; out.h = srcH_; }
    else { out.w = outW_; out.h = outH_; out.px.resize((size_t)outW_ * outH_); pix::scaleNearest(tmp_.data(), srcW_, srcH_, out.px.data(), outW_, outH_); }
    return true;
  }

  std::unique_ptr<MfSession> ms_;
  IMFSourceReader* reader_ = nullptr;
  Fmt fmt_ = Fmt::Other; int srcW_ = 0, srcH_ = 0, stride_ = 0, outW_ = 0, outH_ = 0; bool bottomUp_ = false;
  std::vector<uint32_t> tmp_; double duration_ = 0; bool eof_ = true;
};

} // namespace
std::unique_ptr<IVideoDecoder> makeVideoDecoder() { return std::unique_ptr<IVideoDecoder>(new MfVideoDecoder()); }
}} // namespace
