// MOD-11/12 audio encoder seam (additive). Used by broadcast (RTMP needs raw AAC + AudioSpecificConfig). No portable AAC encoder
// exists in this build; the Windows Media Foundation AAC adapter (platform/windows) implements it and reports EncoderUnavailable
// honestly when the MFT is missing.
#pragma once
#include "core/types.h"
#include <string>
#include <vector>
namespace ss {
struct AudioEncoderProfile { uint32_t rate = 48000, channels = 2; int bitrateKbps = 128; };
struct EncodedAudioPacket { Ns pts = 0; std::vector<uint8_t> data; };   // one raw AAC access unit (no ADTS)
struct IAudioEncoder {
  virtual ~IAudioEncoder() = default;
  virtual Status open(const AudioEncoderProfile&) = 0;
  // Interleaved float32 in [-1,1]. `pts` is the time of the first frame. Output packets (0..n) are appended to `out`; the encoder buffers
  // partial frames internally (AAC-LC frame = 1024 samples), pts of an output packet is derived from the sample count.
  virtual Status encode(const float* interleaved, size_t frames, Ns pts, std::vector<EncodedAudioPacket>& out) = 0;
  virtual void close() = 0;
  virtual std::vector<uint8_t> audioSpecificConfig() const = 0;   // valid after open()
  virtual std::string name() const = 0;
};
}  // namespace ss
