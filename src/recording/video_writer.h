// MOD-11 segmented A/V container writers (spec §12).
//   * AVI  : classic RIFF AVI 1.0 (OpenDML-free), < 2 GiB per segment (budget clamped to 1.9 GiB), MJPEG video + PCM audio,
//            idx1 written on finalize; a truncated file is repairable by scanning chunks (recoverAvi). Fixed frame rate:
//            skipped slots are filled with zero-length '00dc' chunks (AVI "repeat previous frame"), counted as duplicates.
//   * MKV  : EBML/Matroska, SeekHead, Info, Tracks, Clusters <= 5 s with SimpleBlocks (V_MJPEG + A_PCM/INT/LIT), Tags,
//            Cues at finalize. Variable frame rate (no duplicates). Clusters/Segment are written unknown-size first and
//            patched, so a crash leaves a streaming-valid file that tolerant readers can scan (no Cues in that case).
//   * Segmentation: 15 min or size budget, whichever first. Audio splits at the exact sample (contiguous across segments,
//            no loss/overlap, enforced); video rolls at the next key frame. Journal lists active/completed segments.
//   * H.264 is not produced here: see recording/video_encoder.h (EncoderRegistry reports EncoderUnavailable honestly).
#pragma once
#include "recording/io.h"
#include "recording/journal.h"
#include "recording/sync.h"
#include "recording/video_encoder.h"
#include <utility>
namespace ss {

enum class MediaContainer : uint8_t { Avi, Mkv };
const char* containerExt(MediaContainer c);

struct MediaWriterConfig {
  std::string dir, base;                         // base from rec::baseName(); "_0000.<ext>" appended per segment
  bool hasAudio = true;
  uint32_t audioRate = 48000, audioChannels = 2; uint16_t audioBits = 24;   // 16 or 24
  VideoCodec codec = VideoCodec::MJPEG;          // only MJPEG is writable here (FourCC 'MJPG' / V_MJPEG)
  int width = 800, height = 600, fpsNum = 25, fpsDen = 1;
  Ns maxSegmentNs = 15ll * 60 * kNsPerSec;
  uint64_t maxSegmentBytes = 1536ull << 20;      // AVI clamps to 1.9 GiB, MKV to 3 GiB
  uint64_t firstSample = 0;                      // session audio sample index of the first sample
  std::string writingApp = "StageSignal";
};

class MediaWriter {
 public:
  using Tags = std::vector<std::pair<std::string, std::string>>;
  using TagProvider = std::function<Tags(const SegmentInfo&)>;   // MKV: called at segment finalize
  static std::unique_ptr<MediaWriter> create(MediaContainer c, IFileSystem& fs, SegmentJournal* j = nullptr);
  virtual ~MediaWriter();
  Status open(const MediaWriterConfig& cfg);
  // Audio must be contiguous: the writer owns the next-sample counter (silence for gaps is inserted by the caller).
  Status writeAudio(const float* interleaved, size_t frames);
  // pts = session timeline ns (SyncTimeline::videoToTimeline). Frames older than their segment start clamp to 0 (counted).
  Status writeVideo(Ns pts, const uint8_t* data, size_t n, bool key);
  void setTagProvider(TagProvider p) { tags_ = std::move(p); }
  Status close();                                // finalize active segment; idempotent
  const std::vector<SegmentInfo>& segments() const { return segs_; }
  uint64_t bytesWritten() const { uint64_t t = 0; for (auto& x : segs_) if (x.complete) t += x.bytes; return t + (sink_ ? sink_->size() : 0); }
  uint64_t nextSample() const { return nextSample_; }          // audio samples accepted so far (+firstSample)
  uint64_t durableAudioFrames() const { return durableAudio_; }
  uint64_t clampedSamples() const { return clamped_; }
  uint64_t droppedVideo() const { return droppedVideo_; }      // fixed-fps slot collisions
  uint64_t duplicatedVideo() const { return dupVideo_; }       // zero-length repeat slots (AVI)
  uint64_t earlyVideo() const { return earlyVideo_; }          // frames clamped to segment start
  bool failed() const { return failed_; }
  Status lastError() const { return err_; }
 protected:
  MediaWriter(IFileSystem& fs, SegmentJournal* j) : fs_(fs), j_(j) {}
  virtual const char* ext() const = 0;
  virtual bool fixedFps() const = 0;
  virtual uint64_t hardLimitBytes() const = 0;                 // absolute container limit for a segment
  virtual Status beginSegment(IFileSink& sink, const SegmentInfo& seg) = 0;
  virtual Status putAudio(const uint8_t* pcm, size_t frames, uint64_t segFrameIndex) = 0;
  // slot/dupBefore valid only when fixedFps(); segRelNs is time since segment start.
  virtual Status putVideo(Ns segRelNs, int64_t slot, uint32_t dupBefore, const uint8_t* d, size_t n, bool key) = 0;
  virtual Status endSegment(SegmentInfo& seg) = 0;             // flush pending, trailer, header patches
  virtual uint64_t projectedBytes(uint64_t addPayload) const = 0;   // bytes if finalized right after adding payload
  IFileSystem& fs_; SegmentJournal* j_; MediaWriterConfig cfg_; std::unique_ptr<IFileSink> sink_;
  TagProvider tags_;
  uint64_t bytesPerFrame() const { return (uint64_t)cfg_.audioChannels * (cfg_.audioBits / 8); }
 private:
  Status startSegment(Ns startRelHint);
  Status finishSegment(bool clean);
  Status fail(const Status& s);
  std::vector<SegmentInfo> segs_; FrameCadence cad_;
  uint64_t nextSample_ = 0, segAudio_ = 0, segVideoSlots_ = 0, durableAudio_ = 0, clamped_ = 0, droppedVideo_ = 0, dupVideo_ = 0, earlyVideo_ = 0, audioCapFrames_ = 0;
  Ns segStartNs_ = 0; bool inSeg_ = false, opened_ = false, closed_ = false, failed_ = false; Status err_;
  std::vector<uint8_t> scratch_;
};

struct AviRecovery {
  bool ok = false, repaired = false; uint64_t videoSlots = 0, audioFrames = 0, droppedTailBytes = 0; std::string detail;
};
// Rebuilds idx1 + sizes of a truncated/unfinalized AVI written by this module by scanning movi chunks.
AviRecovery recoverAvi(IFileSystem& fs, const std::string& path);

} // namespace ss
