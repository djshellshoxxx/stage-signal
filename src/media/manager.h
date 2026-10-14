// MOD-07 MediaManager: asynchronous media loading/decoding on a dedicated in-process "media worker" thread.
//
// Threading (spec §8: "media worker owns decoder/encoder/file/network handles"):
//  - request()/frame()/state()/stats() are called from control/UI/render threads. They take only short, bounded
//    critical sections and NEVER decode, read files, or wait for the worker (REQ-S08).
//  - One worker thread performs all file IO, stb/codec decode, cropping and video stepping. A supervisor thread
//    (watchdog) detects a worker that threw out of its loop, died, or hung past workerTimeoutMs; it fails the
//    in-flight source (placeholder substituted), abandons that thread and starts a fresh worker. The queue and the
//    cache survive; the host, audio and current scene keep running (REQ-X03).
//  - This is the IN-PROCESS media worker. Moving the same job loop into a separate low-privilege process is an optional
//    extra isolation layer (spec §8 "one optional media/encoding worker process"); it is NOT claimed here. In-process, a
//    non-cooperative hang cannot be killed - the stuck thread is orphaned (leaked) and replaced, and a native crash
//    (SIGSEGV) in a third-party decoder would still take the process down. Exceptions of any type are contained.
//
// Memory: ONE accounting number (cache bytes) covers decoded stills, GIF frames and reserved video buffers, including
// payloads still pinned by handles after LRU eviction. It never exceeds the budget (BASE 8 MiB, spec §13). Transient
// decode buffers inside the worker are bounded by ImportLimits and are not part of the cache number.
#pragma once
#include "core/ports.h"
#include "media/import.h"
#include "media/decoder_registry.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ss { namespace media {

// ---- worker fault-injection seam (REQ-X03 / T-FAULT) -------------------------------------------------------------
enum class FaultKind : uint8_t {
  None,
  Throw,        // decoder throws std::runtime_error
  ThrowNonStd,  // decoder throws a non-std type (int)
  BadAlloc,     // decoder throws std::bad_alloc
  WorkerDie,    // the worker thread terminates abnormally with the job in flight (simulated process death)
  Hang          // the decoder never returns (cooperatively released on abandon/shutdown so tests do not leak)
};
// Called on the worker thread before each load job and before each video decode step; `assetId` identifies the source.
using FaultHook = std::function<FaultKind(const std::string& assetId)>;

struct MediaConfig {
  Tier tier = Tier::Base;
  uint64_t cacheBudgetBytes = 0;     // 0 -> tier default (BASE 8 MiB, STANDARD 64 MiB)
  size_t queueCapacity = 16;         // bounded request queue; overflow is rejected, never blocks the caller
  int maxVideoSources = -1;          // -1 -> tier default (BASE 0, STANDARD 1)  (spec §13)
  int videoMaxW = 0, videoMaxH = 0;  // 0 -> tier default (BASE 640x360 [unused: video disabled], STANDARD 800x600)
  int placeholderW = 320, placeholderH = 240;
  uint32_t matteRgb = 0x000000;      // transparency is flattened onto this (IF-07 Frame has no alpha channel)
  int64_t workerTimeoutMs = 3000;    // a single job/decode step longer than this is declared hung
  int64_t watchdogPollMs = 10;
  int64_t failureRetryMs = 2000;     // failed sources are negative-cached this long (avoids a decode storm)
  int maxVideoFramesPerTick = 8;     // catch-up bound per worker pass (late frames are skipped, not queued)
  const IClock* clock = nullptr;     // default systemClock()
};

struct AssetSpec {
  std::string id;                    // logical asset id used by request()
  std::string path;                  // local path only (validateLocalPath)
  CropRect crop;                     // normalized crop (stills/GIF at load; video per decoded frame)
  bool loop = true;                  // GIF/video looping
  double speed = 1.0;                // initial playback speed (GIF 0.25-4, video 0.25-2; clamped)
};

struct MediaStats {
  uint64_t requests = 0, cacheHits = 0, loadsOk = 0, loadsFailed = 0, queueRejects = 0;
  uint64_t workerRestarts = 0, hungJobs = 0, workerDeaths = 0, evictions = 0;
  uint64_t cacheBytes = 0, cachePeakBytes = 0, cacheBudget = 0;
  size_t queueDepth = 0, cachedItems = 0, activeVideos = 0;
};

namespace detail { struct Impl; struct Ticket; struct Payload; struct VideoSession; }

class MediaHandle {
 public:
  enum class State : uint8_t { Pending, Ready, Failed };
  ~MediaHandle();
  State state() const;                       // Failed also when a video source failed after becoming Ready
  Status status() const;                     // failure reason (sanitized) or Ok
  bool isPlaceholder() const { return state() == State::Failed; }
  const std::string& assetId() const { return id_; }
  // Frame for media-clock time `elapsedNs` since this source started. Single consumer thread (render) per handle.
  //  Pending -> nullptr (keep showing the previous content); Failed -> the documented "MISSING MEDIA" checkerboard;
  //  Ready still -> the image; GIF -> frame by cumulative clamped delays (loop or hold last); video -> newest decoded
  //  frame with pts <= target (nullptr until the first frame arrives). Never blocks, never decodes.
  std::shared_ptr<const Frame> frame(Ns elapsedNs);
  void setSpeed(double s);                   // clamped to the source's range; applies from the next frame() call
  void setLoop(bool l);                      // GIF and video looping
  int width() const; int height() const;     // 0 until Ready
  std::vector<std::string> warnings() const; // e.g. GIF delay clamps, BASE downscale notice
  size_t frameCount() const;                 // GIF frame count (1 for stills, 0 otherwise)
 private:
  friend class MediaManager;
  MediaHandle(std::shared_ptr<const Frame> placeholder, std::shared_ptr<detail::Ticket> t, const AssetSpec& spec);
  std::shared_ptr<const Frame> ph_; std::shared_ptr<detail::Ticket> t_;
  std::string id_; bool loop_; double speed_;
  Ns lastElapsed_ = 0; double mediaNs_ = 0; bool started_ = false;
};
using MediaHandlePtr = std::shared_ptr<MediaHandle>;

class MediaManager {
 public:
  explicit MediaManager(const MediaConfig& cfg = MediaConfig());
  ~MediaManager();                           // stops workers; never blocks on a hung decoder (orphans it after ~1 s)
  MediaManager(const MediaManager&) = delete; MediaManager& operator=(const MediaManager&) = delete;

  // Validates id/path/extension/crop only (no IO). Replaces an existing registration with the same id.
  Status registerAsset(const AssetSpec& spec);
  // Asynchronous load. Always returns a handle immediately; failures are visible via state()/frame() (placeholder).
  MediaHandlePtr request(const std::string& assetId);
  void invalidate(const std::string& assetId);   // drop cache entry + failure record so the next request reloads

  void setDecoderRegistry(const DecoderRegistry& r);   // snapshot copy; availability comes from the package manifest
  const MediaConfig& config() const { return cfg_; }
  MediaStats stats() const;
  void setFaultHook(FaultHook h);            // seam: simulate decoder crash/throw/hang (tests / T-FAULT)
 private:
  MediaConfig cfg_;
  std::shared_ptr<detail::Impl> impl_;
};

}} // namespace ss::media
