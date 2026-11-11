// Windows Media Foundation encoder adapters (loaded at run time; the exe starts without them):
//   * H.264 via the Microsoft software H.264 encoder MFT (CLSID_CMSH264EncoderMFT): baseline, CBR, 2 s GOP, low latency, NV12 input
//     (converted from 0x00RRGGBB in scalar code). Output = Annex B access units; codecPrivate() = avcC from the MFT's sequence header.
//   * AAC-LC via CLSID_AACMFTEncoder with raw payload; AudioSpecificConfig is computed from rate/channels.
// HONEST STATUS: cross-compiled only. Neither MFT exists under wine, so the encode path has NOT been executed in this environment;
// only the pure helpers (RGB->NV12, avcC/ASC building) are tested. Registered as qualified=false. Patent/redistribution decisions
// for H.264/AAC are not made here (spec §12/§16).
#include "mf_common.h"
#include "pixconv.h"
#include "recording/h264.h"
#include <mftransform.h>
#include <codecapi.h>
#include <cstring>

namespace ss { namespace win {
using namespace detail;
namespace {

GUID mkGuid(uint32_t a, uint16_t b, uint16_t c, uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4, uint8_t d5, uint8_t d6, uint8_t d7) {
  GUID g; g.Data1 = a; g.Data2 = b; g.Data3 = c; g.Data4[0] = d0; g.Data4[1] = d1; g.Data4[2] = d2; g.Data4[3] = d3; g.Data4[4] = d4; g.Data4[5] = d5; g.Data4[6] = d6; g.Data4[7] = d7; return g;
}
const GUID kClsidH264Enc = mkGuid(0x6ca50344, 0x051a, 0x4ded, 0x97, 0x79, 0xa4, 0x33, 0x05, 0x16, 0x5e, 0x35);   // CLSID_CMSH264EncoderMFT
const GUID kClsidAacEnc = mkGuid(0x93af0c51, 0x2275, 0x45d2, 0xa3, 0x5b, 0xf2, 0xba, 0x21, 0xca, 0xed, 0x00);    // CLSID_AACMFTEncoder
const GUID kIidCodecApi = mkGuid(0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda);    // IID_ICodecAPI (libuuid does not export it)
const GUID kCapRateMode = mkGuid(0x1c0608e9, 0x370c, 0x4710, 0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23);
const GUID kCapMeanBitrate = mkGuid(0xf7222374, 0x2144, 0x4815, 0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52);
const GUID kCapGop = mkGuid(0x95f31b26, 0x95a4, 0x41aa, 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1);
const GUID kCapLowLatency = mkGuid(0x9d3ecd55, 0x89e8, 0x490a, 0x97, 0x0a, 0x0c, 0x95, 0x48, 0xd5, 0xa5, 0x6e);
const GUID kCapBFrames = mkGuid(0x8d390aac, 0xdc5c, 0x4200, 0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2);

typedef HRESULT (WINAPI *PFN_MFCreateSample)(IMFSample**);
typedef HRESULT (WINAPI *PFN_MFCreateMemoryBuffer)(DWORD, IMFMediaBuffer**);

struct MfEnc {
  PFN_MFCreateSample createSample = nullptr; PFN_MFCreateMemoryBuffer createBuffer = nullptr;
  bool ok() const { return createSample && createBuffer; }
};
MfEnc mfEnc() {
  MfEnc e; const MfApi& a = mfApi();
  e.createSample = procAddr<PFN_MFCreateSample>(a.plat, "MFCreateSample"); e.createBuffer = procAddr<PFN_MFCreateMemoryBuffer>(a.plat, "MFCreateMemoryBuffer"); return e;
}
HRESULT createMft(const GUID& clsid, IMFTransform** out) {
  return CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFTransform), reinterpret_cast<void**>(out));
}
Status notInstalled(const char* what, HRESULT hr) {
  return Status::Fail(Err::EncoderUnavailable, std::string(what) + " is not available on this Windows installation (" + hrString(hr) + "). It needs Windows 7 or newer with Media Foundation (Windows N/KN editions need the Media Feature Pack).", false);
}
void setU32(ICodecAPI* api, const GUID& g, uint32_t v) { if (!api) return; VARIANT var; VariantInit(&var); var.vt = VT_UI4; var.ulVal = v; api->SetValue(&g, &var); }   // best effort: unsupported knobs are ignored

// ============================== H.264 ==============================
class MfH264 : public IVideoEncoder {
 public:
  ~MfH264() override { close(); }
  Status open(const VideoProfile& p) override {
    close(); comThreadInit();
    if (p.codec != VideoCodec::H264) return Status::Fail(Err::InvalidArgument, "profile codec is not H.264");
    if (p.width < 64 || p.height < 64 || p.width > 4096 || p.height > 4096 || (p.width & 1) || (p.height & 1)) return Status::Fail(Err::InvalidArgument, "H.264 size must be even, 64..4096");
    if (p.fpsNum <= 0 || p.fpsDen <= 0) return Status::Fail(Err::InvalidArgument, "invalid frame rate");
    if (!mediaFoundationAvailable()) return Status::Fail(Err::EncoderUnavailable, "Media Foundation is not available on this Windows version", false);
    ms_.reset(new MfSession()); if (!ms_->ok()) { ms_.reset(); return Status::Fail(Err::EncoderUnavailable, "Media Foundation failed to start", false); }
    mf_ = mfEnc(); if (!mf_.ok()) { close(); return Status::Fail(Err::EncoderUnavailable, "Media Foundation sample functions are missing", false); }
    HRESULT hr = createMft(kClsidH264Enc, mft_.put()); if (FAILED(hr)) { close(); return notInstalled("The Microsoft H.264 encoder", hr); }
    p_ = p; int kbps = p.bitrateKbps > 0 ? p.bitrateKbps : 2500; double fps = (double)p.fpsNum / p.fpsDen;
    ComPtr<ICodecAPI> api; mft_->QueryInterface(kIidCodecApi, reinterpret_cast<void**>(api.put()));
    setU32(api.get(), kCapRateMode, eAVEncCommonRateControlMode_CBR); setU32(api.get(), kCapMeanBitrate, (uint32_t)kbps * 1000);
    setU32(api.get(), kCapGop, (uint32_t)(fps * 2.0 + 0.5)); setU32(api.get(), kCapLowLatency, 1); setU32(api.get(), kCapBFrames, 0);   // 2 s key interval, no reordering
    const MfApi& A = mfApi();
    ComPtr<IMFMediaType> out; ((PFN_MFCreateMediaType)A.fnCreateMediaType)(out.put());
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kbps * 1000); MFSetAttributeSize(out.get(), MF_MT_FRAME_SIZE, (UINT32)p.width, (UINT32)p.height);
    MFSetAttributeRatio(out.get(), MF_MT_FRAME_RATE, (UINT32)p.fpsNum, (UINT32)p.fpsDen); MFSetAttributeRatio(out.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    hr = mft_->SetOutputType(0, out.get(), 0); if (FAILED(hr)) { close(); return Status::Fail(Err::EncoderUnavailable, "H.264 encoder rejected the output type (" + hrString(hr) + ")", false); }
    ComPtr<IMFMediaType> in; ((PFN_MFCreateMediaType)A.fnCreateMediaType)(in.put());
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12); in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in.get(), MF_MT_FRAME_SIZE, (UINT32)p.width, (UINT32)p.height); MFSetAttributeRatio(in.get(), MF_MT_FRAME_RATE, (UINT32)p.fpsNum, (UINT32)p.fpsDen); MFSetAttributeRatio(in.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = mft_->SetInputType(0, in.get(), 0); if (FAILED(hr)) { close(); return Status::Fail(Err::EncoderUnavailable, "H.264 encoder rejected NV12 input (" + hrString(hr) + ")", false); }
    readSequenceHeader();
    MFT_OUTPUT_STREAM_INFO si; ZeroMemory(&si, sizeof si); mft_->GetOutputStreamInfo(0, &si); providesSamples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0; outSize_ = si.cbSize ? si.cbSize : (DWORD)(p.width * p.height);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0); mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    nv12_.assign((size_t)p.width * p.height * 3 / 2, 0); open_ = true; return Status::Ok();
  }
  Status encode(const Frame& f, Ns pts, EncodedPacket& out, bool& have) override {
    have = false; if (!open_) return Status::Fail(Err::InternalFault, "encoder not open");
    comThreadInit();
    if (f.w != p_.width || f.h != p_.height || f.px.size() < (size_t)f.w * f.h) return Status::Fail(Err::InvalidArgument, "frame size does not match encoder profile");
    pix::rgb32ToNv12(f.px.data(), f.w, f.h, nv12_.data(), f.w, nv12_.data() + (size_t)f.w * f.h, f.w);
    ComPtr<IMFMediaBuffer> buf; HRESULT hr = mf_.createBuffer((DWORD)nv12_.size(), buf.put()); if (FAILED(hr)) return Status::Fail(Err::InternalFault, "MFCreateMemoryBuffer failed (" + hrString(hr) + ")");
    BYTE* d = nullptr; if (FAILED(buf->Lock(&d, nullptr, nullptr)) || !d) return Status::Fail(Err::InternalFault, "buffer lock failed");
    std::memcpy(d, nv12_.data(), nv12_.size()); buf->Unlock(); buf->SetCurrentLength((DWORD)nv12_.size());
    ComPtr<IMFSample> smp; mf_.createSample(smp.put()); smp->AddBuffer(buf.get()); smp->SetSampleTime(pts / 100); smp->SetSampleDuration((LONGLONG)(1e7 * p_.fpsDen / p_.fpsNum));
    hr = mft_->ProcessInput(0, smp.get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) return Status::Fail(Err::EncoderUnavailable, "H.264 encoder input failed (" + hrString(hr) + ")");
    return drain(out, have);
  }
  void close() override {
    if (mft_) { mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0); }
    mft_.reset(); ms_.reset(); open_ = false; nv12_.clear();
  }
  Ns delayNs() const override { return 0; }   // low-latency mode, no B-frames: output sample time equals input time
  std::string name() const override { return "h264-mf (Microsoft H.264 encoder MFT, software)"; }
  VideoCodec codec() const override { return VideoCodec::H264; }
  std::vector<uint8_t> codecPrivate() const override { return avcC_; }
 private:
  void readSequenceHeader() {
    ComPtr<IMFMediaType> cur; if (FAILED(mft_->GetOutputCurrentType(0, cur.put()))) return;
    UINT32 n = 0; if (FAILED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &n)) || !n || n > 4096) return;
    std::vector<uint8_t> b(n); if (FAILED(cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, b.data(), n, &n))) return;
    avcC_ = h264::avcCFromAnnexB(b.data(), n);
  }
  Status drain(EncodedPacket& out, bool& have) {
    for (int guard = 0; guard < 4; ++guard) {
      MFT_OUTPUT_DATA_BUFFER ob; ZeroMemory(&ob, sizeof ob); ComPtr<IMFSample> own;
      if (!providesSamples_) { ComPtr<IMFMediaBuffer> b; if (FAILED(mf_.createBuffer(outSize_, b.put()))) return Status::Fail(Err::InternalFault, "output buffer allocation failed"); mf_.createSample(own.put()); own->AddBuffer(b.get()); ob.pSample = own.get(); }
      DWORD status = 0; HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return Status::Ok();
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        ComPtr<IMFMediaType> t; if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, t.put()))) mft_->SetOutputType(0, t.get(), 0);
        readSequenceHeader(); continue;
      }
      if (FAILED(hr)) return Status::Fail(Err::EncoderUnavailable, "H.264 encoder output failed (" + hrString(hr) + ")");
      IMFSample* smp = ob.pSample; ComPtr<IMFMediaBuffer> mb; Status r = Status::Ok();
      if (smp && SUCCEEDED(smp->ConvertToContiguousBuffer(mb.put()))) {
        BYTE* d = nullptr; DWORD len = 0;
        if (SUCCEEDED(mb->Lock(&d, nullptr, &len)) && d) {
          out.data.assign(d, d + len); mb->Unlock(); LONGLONG t = 0; smp->GetSampleTime(&t); out.pts = (Ns)t * 100;
          out.key = h264::hasIdr(out.data.data(), out.data.size()); have = true;
          if (avcC_.empty() && out.key) avcC_ = h264::avcCFromAnnexB(out.data.data(), out.data.size());   // fallback when the type carried no header
        }
      }
      if (ob.pEvents) ob.pEvents->Release();
      if (!providesSamples_ || !smp) {} else smp->Release();
      return r;
    }
    return Status::Ok();
  }
  VideoProfile p_; ComPtr<IMFTransform> mft_; std::unique_ptr<MfSession> ms_; MfEnc mf_; bool open_ = false, providesSamples_ = false; DWORD outSize_ = 0;
  std::vector<uint8_t> nv12_, avcC_;
};

// ============================== AAC ==============================
class MfAac : public IAudioEncoder {
 public:
  ~MfAac() override { close(); }
  Status open(const AudioEncoderProfile& p) override {
    close(); comThreadInit();
    if (p.channels < 1 || p.channels > 2) return Status::Fail(Err::InvalidArgument, "AAC encoder supports mono or stereo");
    asc_ = aac::audioSpecificConfig(p.rate, p.channels); if (asc_.empty()) return Status::Fail(Err::InvalidArgument, "unsupported AAC sample rate");
    if (!mediaFoundationAvailable()) return Status::Fail(Err::EncoderUnavailable, "Media Foundation is not available on this Windows version", false);
    ms_.reset(new MfSession()); if (!ms_->ok()) { ms_.reset(); return Status::Fail(Err::EncoderUnavailable, "Media Foundation failed to start", false); }
    mf_ = mfEnc(); if (!mf_.ok()) { close(); return Status::Fail(Err::EncoderUnavailable, "Media Foundation sample functions are missing", false); }
    HRESULT hr = createMft(kClsidAacEnc, mft_.put()); if (FAILED(hr)) { close(); return notInstalled("The Microsoft AAC encoder", hr); }
    p_ = p; const MfApi& A = mfApi();
    static const int rates[4] = {12000, 16000, 20000, 24000};                 // AAC MFT accepts these byte rates (96/128/160/192 kb/s)
    int want = p.bitrateKbps * 125, best = rates[1]; for (int r : rates) if (std::abs(r - want) < std::abs(best - want)) best = r;
    ComPtr<IMFMediaType> out; ((PFN_MFCreateMediaType)A.fnCreateMediaType)(out.put());
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC); out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, p.rate); out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, p.channels); out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)best);
    out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0); out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);   // raw AAC-LC, no ADTS
    hr = mft_->SetOutputType(0, out.get(), 0); if (FAILED(hr)) { close(); return Status::Fail(Err::EncoderUnavailable, "AAC encoder rejected the output type (" + hrString(hr) + ")", false); }
    ComPtr<IMFMediaType> in; ((PFN_MFCreateMediaType)A.fnCreateMediaType)(in.put());
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM); in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, p.rate); in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, p.channels);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, p.channels * 2); in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, p.rate * p.channels * 2);
    hr = mft_->SetInputType(0, in.get(), 0); if (FAILED(hr)) { close(); return Status::Fail(Err::EncoderUnavailable, "AAC encoder rejected 16-bit PCM input (" + hrString(hr) + ")", false); }
    MFT_OUTPUT_STREAM_INFO si; ZeroMemory(&si, sizeof si); mft_->GetOutputStreamInfo(0, &si); providesSamples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0; outSize_ = si.cbSize ? si.cbSize : 8192;
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0); mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    open_ = true; haveBase_ = false; outFrames_ = 0; return Status::Ok();
  }
  Status encode(const float* pcm, size_t frames, Ns pts, std::vector<EncodedAudioPacket>& out) override {
    if (!open_) return Status::Fail(Err::InternalFault, "encoder not open");
    comThreadInit();
    if (!frames) return Status::Ok();
    if (!haveBase_) { haveBase_ = true; basePts_ = pts; }
    size_t n = frames * p_.channels; std::vector<int16_t> s16(n);
    for (size_t i = 0; i < n; ++i) { float v = pcm[i]; if (!(v == v)) v = 0; if (v > 1) v = 1; if (v < -1) v = -1; float x = v * 32767.0f; s16[i] = (int16_t)(x >= 0 ? x + 0.5f : x - 0.5f); }
    ComPtr<IMFMediaBuffer> buf; HRESULT hr = mf_.createBuffer((DWORD)(n * 2), buf.put()); if (FAILED(hr)) return Status::Fail(Err::InternalFault, "MFCreateMemoryBuffer failed");
    BYTE* d = nullptr; if (FAILED(buf->Lock(&d, nullptr, nullptr)) || !d) return Status::Fail(Err::InternalFault, "buffer lock failed");
    std::memcpy(d, s16.data(), n * 2); buf->Unlock(); buf->SetCurrentLength((DWORD)(n * 2));
    ComPtr<IMFSample> smp; mf_.createSample(smp.put()); smp->AddBuffer(buf.get()); smp->SetSampleTime(pts / 100); smp->SetSampleDuration((LONGLONG)(1e7 * frames / p_.rate));
    hr = mft_->ProcessInput(0, smp.get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) return Status::Fail(Err::EncoderUnavailable, "AAC encoder input failed (" + hrString(hr) + ")");
    for (int guard = 0; guard < 64; ++guard) {
      MFT_OUTPUT_DATA_BUFFER ob; ZeroMemory(&ob, sizeof ob); ComPtr<IMFSample> own;
      if (!providesSamples_) { ComPtr<IMFMediaBuffer> b; if (FAILED(mf_.createBuffer(outSize_, b.put()))) return Status::Fail(Err::InternalFault, "output buffer allocation failed"); mf_.createSample(own.put()); own->AddBuffer(b.get()); ob.pSample = own.get(); }
      DWORD status = 0; hr = mft_->ProcessOutput(0, 1, &ob, &status);
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
      if (FAILED(hr)) return Status::Fail(Err::EncoderUnavailable, "AAC encoder output failed (" + hrString(hr) + ")");
      IMFSample* o = ob.pSample; ComPtr<IMFMediaBuffer> mb;
      if (o && SUCCEEDED(o->ConvertToContiguousBuffer(mb.put()))) {
        BYTE* dd = nullptr; DWORD len = 0;
        if (SUCCEEDED(mb->Lock(&dd, nullptr, &len)) && dd && len) {
          EncodedAudioPacket pk; const uint8_t* payload = dd; size_t plen = len; aac::stripAdts(dd, len, payload, plen);   // safety: raw was requested
          pk.data.assign(payload, payload + plen); mb->Unlock();
          pk.pts = basePts_ + (Ns)(outFrames_ * 1024ull * kNsPerSec / p_.rate); ++outFrames_; out.push_back(std::move(pk));
        } else if (dd) mb->Unlock();
      }
      if (ob.pEvents) ob.pEvents->Release();
      if (providesSamples_ && o) o->Release();
    }
    return Status::Ok();
  }
  void close() override { if (mft_) { mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0); } mft_.reset(); ms_.reset(); open_ = false; }
  std::vector<uint8_t> audioSpecificConfig() const override { return asc_; }
  std::string name() const override { return "aac-mf (Microsoft AAC encoder MFT, AAC-LC raw)"; }
 private:
  AudioEncoderProfile p_; ComPtr<IMFTransform> mft_; std::unique_ptr<MfSession> ms_; MfEnc mf_; bool open_ = false, providesSamples_ = false, haveBase_ = false; DWORD outSize_ = 0;
  std::vector<uint8_t> asc_; Ns basePts_ = 0; uint64_t outFrames_ = 0;
};

EncoderAvailability probe(const GUID& clsid, VideoCodec c, const char* nm, const char* what) {
  EncoderAvailability a; a.codec = c; a.name = nm; comThreadInit();
  if (!mediaFoundationAvailable()) { a.reason = "Media Foundation is not available on this Windows version."; return a; }
  ComPtr<IMFTransform> t; HRESULT hr = createMft(clsid, t.put());
  if (FAILED(hr)) { a.reason = std::string("The Microsoft ") + what + " encoder is not installed (" + hrString(hr) + "). Windows N/KN editions need the Media Feature Pack."; return a; }
  a.available = true; a.qualified = false; a.reason = "software MFT, not hardware-tested, not qualified against the encode budget"; return a;
}
} // namespace

std::unique_ptr<IAudioEncoder> makeAacEncoder() { return std::unique_ptr<IAudioEncoder>(new MfAac()); }
EncoderAvailability h264EncoderAvailability() { return probe(kClsidH264Enc, VideoCodec::H264, "h264-mf", "H.264"); }
EncoderAvailability aacEncoderAvailability() { return probe(kClsidAacEnc, VideoCodec::None, "aac-mf", "AAC"); }

void registerWindowsEncoders(EncoderRegistry& r) {
  EncoderAvailability a = h264EncoderAvailability();
  if (!a.available) return;                                // not registered: the registry then reports EncoderUnavailable honestly
  r.add(VideoCodec::H264, "h264-mf", [] { return std::unique_ptr<IVideoEncoder>(new MfH264()); }, false,
        "Microsoft software H.264 MFT (baseline, CBR, 2 s keyframes, low latency); not hardware-tested, qualified=false; codec licensing decision pending");
}

}} // namespace
