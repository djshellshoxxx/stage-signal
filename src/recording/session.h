// MOD-11 RecordingSession (IF-10 recording half; spec §8 queues/gaps, §12 lifecycle, REQ-R01..R06).
//
//  State machine: Idle -> Preparing -> Recording -> Stopping -> Completed; Preparing may end in Failed (actionable Status
//  returned from start(), state NEVER reaches Recording); any active state can reach Failed / Recoverable.
//    Completed   = user stop, everything finalized.
//    Failed      = ended by an error after graceful finalization (disk below reserve, gap policy, clock mismatch ...).
//    Recoverable = a write error left the active segment unfinalized; completed segments are intact, the lost interval is
//                  reported and recoverSession() repairs what is repairable.
//  The profile is immutable for the session. Show/scene changes (noteShowEvent) never touch the session clock.
//  Threading: pushAudio()/pushVideo() are called from capture/render-side threads and NEVER block on disk: bounded rings
//  (processed audio 2 s, render->encoder 3 frames), push-drop-with-counter. One writer thread owns encoder + files
//  (or the caller drives pump() when SessionDeps::manualPump, for deterministic tests).
//  The stream secret is irrelevant here (recording holds no secrets).
#pragma once
#include "recording/io.h"
#include "recording/journal.h"
#include "recording/sync.h"
#include "recording/video_encoder.h"
#include "recording/video_writer.h"
#include "recording/wav_writer.h"

namespace ss {

enum class RecState : uint8_t { Idle, Preparing, Recording, Stopping, Completed, Failed, Recoverable };
const char* recStateName(RecState s);
enum class RecContainer : uint8_t { Wav, Avi, Mkv };
const char* recContainerName(RecContainer c);

constexpr uint64_t kDiskWarnBytes = 2ull << 30;           // warn below 2 GiB ...
constexpr Ns kDiskWarnRemainingNs = 600 * kNsPerSec;      // ... or below ~10 min estimated remaining
constexpr uint64_t kMinStopReserveBytes = 256ull << 20;   // graceful stop reserve = max(256 MiB, est. 30 s)
constexpr Ns kStopReserveNs = 30 * kNsPerSec;
constexpr Ns kGapAllowanceNs = 250 * kNsPerMs;            // report-and-continue allowance per gap
constexpr size_t kAudioQueueMaxBlocks = 1024;

struct RecordingProfile {                                  // immutable once start() is called
  RecContainer container = RecContainer::Wav;              // Wav = audio-only (REQ-R05: needs no encoder/renderer)
  uint32_t audioRate = 48000, audioChannels = 2;
  uint16_t audioBits = 24;                                 // AVI/MKV: 16 or 24; WAV always 24
  VideoProfile video;                                      // used for Avi/Mkv
  Ns segmentNs = 15ll * 60 * kNsPerSec;
  uint64_t segmentBytes = 1ull << 30;
  bool driftCorrection = true; DriftConfig drift;
  bool stopOnExcessiveDrift = true;                        // >200 ppm clock mismatch is an error (spec §12)
  DelayCompensation delay;                                 // dynamics delay is added from DynamicsMeta::delaySamples
  Ns gapAllowanceNs = kGapAllowanceNs; int maxGapsPerMinute = 5;
};
struct DynamicsMeta {                                      // REQ-R02: recorded in metadata, shown in status
  bool enabled = true; bool bypassed = false;
  std::string settingsJson;                                // compressor/limiter settings as JSON text (embedded if valid)
  uint32_t delaySamples = 0;                               // IF-04 sample delay (lookahead + filter group delay)
  std::string ceilingUnit = "sample-peak dBFS";
};
struct SourceStatus {                                      // what the pipeline will actually deliver
  bool audioPresent = false; uint32_t audioRate = 0, audioChannels = 0;
  bool videoPresent = false; int videoW = 0, videoH = 0;
};
struct RecordingRequest {
  RecordingProfile profile;
  std::string dir, showName, sessionId;
  DynamicsMeta dynamics; SourceStatus sources;
};
struct SessionDeps {
  IFileSystem* fs = nullptr; IDiskInfo* disk = nullptr; IClock* clock = nullptr; const EncoderRegistry* encoders = nullptr;
  bool manualPump = false;                                 // tests: no writer thread, caller calls pump()
};

struct GapRecord { uint64_t atSample = 0; uint64_t frames = 0; Ns startNs = 0; Ns durationNs = 0; bool filledWithSilence = true; std::string label; };
struct LostInterval { Ns fromNs = 0, toNs = 0; uint64_t frames = 0; std::string reason; };
struct Marker { uint32_t id = 0; Ns timelineNs = 0; std::string label; };
struct DiskEstimate {
  bool known = false; uint64_t freeBytes = 0; uint64_t reserveBytes = 0;
  double bytesPerSec = 0, bytesPerSecMin = 0, bytesPerSecMax = 0; bool rateMeasured = false;
  double remainingSec = -1, remainingSecLow = -1, remainingSecHigh = -1;   // -1 = unknown; Low/High bound the estimate
  bool warning = false; std::string text;
};
struct RecordingStatus {
  RecState state = RecState::Idle; Status error; std::vector<std::string> warnings;
  DiskEstimate disk;
  uint64_t audioFramesWritten = 0, audioBlocksDropped = 0, audioFramesDropped = 0, videoFramesQueueDropped = 0, videoDropped = 0, videoDuplicated = 0;
  uint64_t gapCount = 0, gapFrames = 0, bytesWritten = 0, queuedAudioFrames = 0, markers = 0, inputBlocks = 0;
  uint64_t nearFullScaleBlocks = 0, possibleClippingBlocks = 0, limiterActiveBlocks = 0; float maxGainReductionDb = 0;
  DriftStats drift; Ns timelineNs = 0;                     // audio timeline position (session clock)
  std::vector<GapRecord> gaps; std::vector<LostInterval> lost; std::vector<SegmentInfo> segments;
  std::string journalPath, metadataPath;
};

class RecordingSession {
 public:
  explicit RecordingSession(const SessionDeps& d);
  ~RecordingSession();                                     // finalizes if still active
  RecordingSession(const RecordingSession&) = delete; RecordingSession& operator=(const RecordingSession&) = delete;

  // Idle/Completed/Failed/Recoverable -> Preparing -> Recording, or a failure Status (state Failed) with an actionable
  // message BEFORE Recording is claimed (REQ-R06).
  Status start(const RecordingRequest& r);
  // Non-blocking. Return false when dropped (queue full / not recording); counters in status().
  bool pushAudio(const AudioBlockPtr& b);
  bool pushVideo(const std::shared_ptr<const Frame>& f);   // Frame::pts = host-clock PTS (same domain as AudioBlock::captureNs)
  Status addMarker(const std::string& label);              // sidecar JSON (+ MKV tag at segment finalize); WAV: sidecar only
  void noteShowEvent(const std::string& what);             // logged in metadata only; the session clock is unaffected
  void noteBlockTelemetry(bool nearFullScale, bool possibleClipping, bool limiterActive, float gainReductionDb);
  void setDynamics(bool bypassed);                         // live bypass toggle is recorded with a timestamp
  Status stop();                                           // idempotent; blocks until finalized (not for audio/render threads)
  RecordingStatus status() const;
  RecState state() const;
  bool pump();                                             // manualPump only: process all queued work; false if nothing done
  void setStateCallback(std::function<void(RecState)> cb); // called on the thread that changed state, without locks held
  const RecordingProfile& profile() const;
 private:
  struct Impl; std::unique_ptr<Impl> p_;
};

struct RecoveryItem { std::string path, kind, detail; bool ok = false, repaired = false; };
// Reads the journal; repairs every segment still listed as active (WAV sizes, AVI idx1). MKV segments are streaming-valid
// (unknown-size Segment/Cluster) and are reported as such, not rewritten.
std::vector<RecoveryItem> recoverSession(IFileSystem& fs, const std::string& journalPath);

} // namespace ss
