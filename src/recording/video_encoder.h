// MOD-11 video encoder seam (spec §12). Honest availability reporting:
//   * MJPEG: built in (stb_image_write JPEG, scalar, no SIMD). This is the "MJPEG fallback" profile. It is NOT a bandwidth-
//     efficient codec: expect roughly 10-40x the disk rate of H.264 at similar quality (see estimatedBitrate()).
//   * H.264: NOT bundled. It needs an external encoder (Windows Media Foundation H.264 MFT adapter, or a qualified FFmpeg
//     hardware/native build) behind IVideoEncoder, registered with EncoderRegistry::add() at startup by another module.
//     Until one is registered, create(H264) returns Err::EncoderUnavailable with an actionable explanation. x264 (GPL) is
//     not used. Patent/redistribution decisions for H.264 are separate (spec §12/§16) and not made here.
#pragma once
#include "core/types.h"
#include <map>
namespace ss {

enum class VideoCodec : uint8_t { None, MJPEG, H264 };
const char* codecName(VideoCodec c);

struct VideoProfile {
  VideoCodec codec = VideoCodec::MJPEG;
  int width = 800, height = 600;
  int fpsNum = 25, fpsDen = 1;
  int quality = 80;                     // MJPEG JPEG quality 1..100 (stb: <=90 uses 4:2:0 chroma subsampling)
  int bitrateKbps = 0;                  // H.264 target (ignored by MJPEG)
  bool operator==(const VideoProfile& o) const { return codec == o.codec && width == o.width && height == o.height && fpsNum == o.fpsNum && fpsDen == o.fpsDen && quality == o.quality && bitrateKbps == o.bitrateKbps; }
};
struct EncodedPacket { Ns pts = 0; bool key = true; std::vector<uint8_t> data; };

struct IVideoEncoder {
  virtual ~IVideoEncoder() = default;
  virtual Status open(const VideoProfile&) = 0;
  // One in, at most one out (MJPEG). Encoders with reorder delay must stamp packets with the source pts they carry and
  // report their fixed latency through delayNs() so SyncTimeline can compensate. Returns false in `have` when buffered.
  virtual Status encode(const Frame& f, Ns pts, EncodedPacket& out, bool& have) = 0;
  virtual void close() = 0;
  virtual Ns delayNs() const = 0;
  virtual std::string name() const = 0;
  virtual VideoCodec codec() const = 0;
};

struct EncoderAvailability {
  VideoCodec codec = VideoCodec::None; std::string name; bool available = false; bool qualified = false; std::string reason;
};

class EncoderRegistry {
 public:
  using Factory = std::function<std::unique_ptr<IVideoEncoder>()>;
  // `qualified` = passed the project's encode-budget qualification on the target tier; the built-in MJPEG is registered
  // as available but qualified=false (fallback profile, qualification is a benchmark outcome, not assumed).
  void add(VideoCodec c, const std::string& name, Factory f, bool qualified, const std::string& note = "");
  static EncoderRegistry withBuiltins();            // MJPEG registered, H.264 reported unavailable
  EncoderAvailability availability(VideoCodec c) const;
  std::vector<EncoderAvailability> list() const;
  // EncoderUnavailable (with reason) when no factory for the codec; otherwise opens an encoder for the profile.
  Status create(const VideoProfile& p, std::unique_ptr<IVideoEncoder>& out) const;
 private:
  struct Entry { std::string name; Factory f; bool qualified; std::string note; };
  std::map<int, Entry> e_;
};

std::unique_ptr<IVideoEncoder> makeMjpegEncoder();
// Rough disk rate estimate (bytes/s) for planning before measurement exists (MJPEG ~ 0.08-0.25 bit/pixel/frame at q80).
double estimatedVideoBytesPerSec(const VideoProfile& p);

} // namespace ss
