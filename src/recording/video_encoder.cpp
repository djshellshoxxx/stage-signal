#include "recording/video_encoder.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC      // private copy: avoids duplicate symbols with other modules that also compile stb
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "stb/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
namespace ss {

const char* codecName(VideoCodec c) { return c == VideoCodec::MJPEG ? "MJPEG" : (c == VideoCodec::H264 ? "H.264" : "none"); }

namespace {
void jpgSink(void* ctx, void* data, int size) { auto* v = (std::vector<uint8_t>*)ctx; v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size); }
class Mjpeg : public IVideoEncoder {
 public:
  Status open(const VideoProfile& p) override {
    if (p.codec != VideoCodec::MJPEG) return Status::Fail(Err::InvalidArgument, "profile codec is not MJPEG");
    if (p.width < 16 || p.height < 16 || p.width > 4096 || p.height > 4096) return Status::Fail(Err::InvalidArgument, "MJPEG size out of range (16..4096)");
    if (p.fpsNum <= 0 || p.fpsDen <= 0) return Status::Fail(Err::InvalidArgument, "invalid frame rate");
    p_ = p; if (p_.quality < 1) p_.quality = 1; if (p_.quality > 100) p_.quality = 100;
    rgb_.assign((size_t)p.width * p.height * 3, 0); open_ = true; return Status::Ok();
  }
  Status encode(const Frame& f, Ns pts, EncodedPacket& out, bool& have) override {
    have = false;
    if (!open_) return Status::Fail(Err::InternalFault, "encoder not open");
    if (f.w != p_.width || f.h != p_.height || f.px.size() < (size_t)f.w * f.h) return Status::Fail(Err::InvalidArgument, "frame size does not match encoder profile (profile is immutable during a session)");
    uint8_t* o = rgb_.data(); const uint32_t* s = f.px.data(); size_t n = (size_t)f.w * f.h;
    for (size_t i = 0; i < n; ++i) { uint32_t p = s[i]; *o++ = (uint8_t)(p >> 16); *o++ = (uint8_t)(p >> 8); *o++ = (uint8_t)p; }
    out.data.clear(); out.data.reserve(n / 4);
    if (!stbi_write_jpg_to_func(jpgSink, &out.data, f.w, f.h, 3, rgb_.data(), p_.quality)) return Status::Fail(Err::InternalFault, "JPEG encode failed");
    out.pts = pts; out.key = true; have = true; return Status::Ok();
  }
  void close() override { open_ = false; rgb_.clear(); }
  Ns delayNs() const override { return 0; }
  std::string name() const override { return "builtin-mjpeg(stb_image_write)"; }
  VideoCodec codec() const override { return VideoCodec::MJPEG; }
 private:
  VideoProfile p_; std::vector<uint8_t> rgb_; bool open_ = false;
};
} // namespace

std::unique_ptr<IVideoEncoder> makeMjpegEncoder() { return std::unique_ptr<IVideoEncoder>(new Mjpeg()); }

void EncoderRegistry::add(VideoCodec c, const std::string& n, Factory f, bool q, const std::string& note) { e_[(int)c] = Entry{n, std::move(f), q, note}; }
EncoderRegistry EncoderRegistry::withBuiltins() {
  EncoderRegistry r;
  r.add(VideoCodec::MJPEG, "builtin-mjpeg", [] { return makeMjpegEncoder(); }, false,
        "software MJPEG fallback; high disk rate; qualification against the frame budget is a benchmark result, not assumed");
  return r;
}
EncoderAvailability EncoderRegistry::availability(VideoCodec c) const {
  EncoderAvailability a; a.codec = c;
  auto it = e_.find((int)c);
  if (it == e_.end() || !it->second.f) {
    a.name = "none"; a.available = false;
    if (c == VideoCodec::H264)
      a.reason = "No H.264 encoder is installed. H.264 needs an external encoder (Windows Media Foundation H.264 adapter or a qualified FFmpeg build) that this build does not bundle. Use audio-only recording or the MJPEG fallback profile.";
    else a.reason = std::string("No encoder registered for ") + codecName(c) + ".";
    return a;
  }
  a.name = it->second.name; a.available = true; a.qualified = it->second.qualified; a.reason = it->second.note; return a;
}
std::vector<EncoderAvailability> EncoderRegistry::list() const {
  std::vector<EncoderAvailability> v; v.push_back(availability(VideoCodec::H264)); v.push_back(availability(VideoCodec::MJPEG)); return v;
}
Status EncoderRegistry::create(const VideoProfile& p, std::unique_ptr<IVideoEncoder>& out) const {
  EncoderAvailability a = availability(p.codec);
  if (!a.available) return Status::Fail(Err::EncoderUnavailable, a.reason, true);
  auto it = e_.find((int)p.codec);
  std::unique_ptr<IVideoEncoder> enc = it->second.f();
  if (!enc) return Status::Fail(Err::EncoderUnavailable, "encoder factory returned nothing for " + a.name);
  Status s = enc->open(p);
  if (!s.ok()) return s.code == Err::InvalidArgument ? s : Status::Fail(Err::EncoderUnavailable, a.name + " failed to open: " + s.detail, s.recoverable);
  out = std::move(enc); return Status::Ok();
}
double estimatedVideoBytesPerSec(const VideoProfile& p) {
  double px = (double)p.width * p.height, fps = (double)p.fpsNum / p.fpsDen;
  if (p.codec == VideoCodec::H264) return p.bitrateKbps > 0 ? p.bitrateKbps * 125.0 : px * fps * 0.1 / 8.0;
  double bpp = 0.05 + 0.0030 * p.quality;   // ~0.29 bit/px at q80 (planning estimate only; real rate is measured)
  return px * fps * bpp / 8.0;
}

} // namespace ss
