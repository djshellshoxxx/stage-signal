// MOD-11 audio-only recording: 24-bit PCM, classic RIFF/WAVE (<= 2 channels), segmented, header-recoverable (REQ-R05).
//  * Segment boundary = min(maxSegmentNs, (maxSegmentBytes-44)/blockAlign) frames, split at the exact sample: every sample
//    lands in exactly one segment, in order (no loss, no overlap); segment i+1.firstSample == segment i end sample.
//  * RIFF/data sizes are patched every headerUpdateFrames and at close, so a crash loses at most the unpatched tail
//    from the *declared* length; recoverWav() re-derives sizes from the real file length (only complete frames kept).
//  * RF64 is intentionally not implemented (spec: only after reader-compat tests): maxSegmentBytes is clamped < 4 GiB.
#pragma once
#include "recording/io.h"
#include "recording/journal.h"
namespace ss {

struct WavConfig {
  uint32_t rate = 48000;
  uint32_t channels = 2;                                  // 1 or 2
  Ns maxSegmentNs = 15ll * 60 * kNsPerSec;                // 15 min
  uint64_t maxSegmentBytes = 1ull << 30;                  // size budget (clamped to 3 GiB)
  uint64_t headerUpdateFrames = 0;                        // 0 => rate (1 s)
  std::string dir;
  std::string base;                                       // from rec::baseName(); segment index + .wav appended
  Ns originNs = 0;                                        // session-timeline time of sample 0 of the first segment
  uint64_t firstSample = 0;                               // session sample index of the first sample written
};

struct WavRecovery {
  bool ok = false; bool repaired = false;
  uint64_t frames = 0; uint64_t dataBytes = 0; uint64_t droppedTailBytes = 0;
  std::string detail;
};
// Repairs RIFF/data sizes of a (possibly crash-truncated) WAV written by WavWriter.
WavRecovery recoverWav(IFileSystem& fs, const std::string& path);

class WavWriter {
 public:
  WavWriter(IFileSystem& fs, SegmentJournal* journal = nullptr) : fs_(fs), j_(journal) {}
  ~WavWriter();
  Status open(const WavConfig& cfg);
  Status write(const float* interleaved, size_t frames);   // fails sticky after the first error; see lostFromSample()
  Status close();                                          // finalizes active segment (idempotent)
  const std::vector<SegmentInfo>& segments() const { return segs_; }
  uint64_t totalFrames() const { return total_; }          // accepted into files (declared in headers once patched)
  uint64_t durableFrames() const { return durable_; }      // frames covered by the last header patch + flush
  uint64_t clampedSamples() const { return clamped_; }
  bool failed() const { return failed_; }
  Status lastError() const { return err_; }
  // First session sample index that was NOT fully written after a failure (valid when failed()).
  uint64_t lostFromSample() const { return cfg_.firstSample + total_; }
  static constexpr size_t kHeaderBytes = 44;
 private:
  Status openSegment();
  Status closeSegment(bool clean);
  Status patchHeader();
  Status fail(const Status& s);
  IFileSystem& fs_; SegmentJournal* j_;
  WavConfig cfg_; std::unique_ptr<IFileSink> sink_;
  std::vector<SegmentInfo> segs_;
  uint64_t segFrames_ = 0, segCap_ = 0, sinceHdr_ = 0, total_ = 0, durable_ = 0, clamped_ = 0, hdrEvery_ = 0;
  bool opened_ = false, failed_ = false, closed_ = false; Status err_;
  std::vector<uint8_t> scratch_;
};

} // namespace ss
