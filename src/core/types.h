// StageSignal core shared types (spec §10). Portable C++17, no Windows/COM/FFmpeg types.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>

namespace ss {

// ---- Errors (spec §10 error categories) -------------------------------------------------
enum class Err : uint8_t {
  Ok, InvalidArgument, UnsupportedCapability, PermissionDenied, DeviceLost, TimestampInvalid,
  ResourceExhausted, DecodeFailure, EncoderUnavailable, DiskFull, TransportFailure,
  SchemaUnsupported, Cancelled, InternalFault
};
const char* errName(Err e);   // defined in core/clock.cpp

struct Status {
  Err code = Err::Ok;
  bool recoverable = true;
  std::string detail;          // sanitized: never contains secrets/URLs with keys
  uint64_t revision = 0;       // originating revision (show/config), 0 if n/a
  bool ok() const { return code == Err::Ok; }
  static Status Ok() { return {}; }
  static Status Fail(Err c, std::string d, bool rec = true) { Status s; s.code = c; s.detail = std::move(d); s.recoverable = rec; return s; }
};

// ---- Time: integer nanoseconds (spec §10) ------------------------------------------------
using Ns = int64_t;
constexpr Ns kNsPerSec = 1000000000LL;
constexpr Ns kNsPerMs  = 1000000LL;
inline Ns msToNs(double ms) { return (Ns)(ms * 1e6); }
inline double nsToMs(Ns n) { return (double)n / 1e6; }
inline double nsToSec(Ns n) { return (double)n / 1e9; }

// Monotonic clock abstraction (injectable for tests).
struct IClock {
  virtual ~IClock() = default;
  virtual Ns nowNs() const = 0;            // monotonic
  virtual int64_t wallUnixMs() const = 0;  // wall clock, for journal/file names only
};
IClock& systemClock();                     // steady_clock based
struct FakeClock : IClock {                // tests/simulation
  Ns t = 0; int64_t wall = 0;
  Ns nowNs() const override { return t; }
  int64_t wallUnixMs() const override { return wall; }
  void advance(Ns d) { t += d; wall += d / kNsPerMs; }
};

// ---- Audio (IF-01) -----------------------------------------------------------------------
enum AudioFlags : uint32_t { AF_Discontinuity = 1, AF_Silent = 2, AF_TimestampValid = 4 };
struct AudioBlock {
  uint32_t rate = 48000;
  uint32_t channels = 2;
  uint64_t firstSample = 0;                // sample index since stream start (per generation)
  Ns captureNs = 0;                        // QPC-derived host time of first sample
  uint32_t flags = 0;
  uint32_t generation = 0;                 // incremented on device reopen; stale generations dropped
  uint64_t sequence = 0;
  std::vector<float> samples;              // interleaved float32, frames = samples.size()/channels
  size_t frames() const { return channels ? samples.size() / channels : 0; }
};
using AudioBlockPtr = std::shared_ptr<const AudioBlock>;

// ---- Features (IF-03) --------------------------------------------------------------------
constexpr float kDbSilence = -120.0f;
struct Estimate {                          // BPM / key with confidence; Unknown != 0
  bool valid = false;
  float confidence = 0;                    // 0..1
  Ns ageNs = 0;                            // age since last update
  double value = 0;                        // BPM for tempo; pitch class 0-11 for key
  bool minor = false;                      // key only
  enum Source : uint8_t { Estimated, Manual, Tap } source = Estimated;
};
struct FeatureSnapshot {
  Ns sourceNs = 0;
  float rmsDb[2] = {kDbSilence, kDbSilence};     // per channel (L,R), dBFS
  float peakDb[2] = {kDbSilence, kDbSilence};
  float rmsDbMix = kDbSilence;
  float band[3] = {0, 0, 0};                     // low/mid/high energy, linear 0..1 normalized (log-compressed)
  float bandDb[3] = {kDbSilence, kDbSilence, kDbSilence};
  float onsetStrength = 0;                       // 0..1 smoothed
  bool onset = false;                            // true on the snapshot in which a new onset fired
  uint64_t onsetCount = 0;
  Estimate tempo, key;
  bool inputNearFullScale = false;               // >= -0.5 dBFS
  bool possibleClipping = false;                 // consecutive near-full-scale samples
  bool silent = true;
  uint64_t sequence = 0;
};

// ---- Frames (IF-07) ----------------------------------------------------------------------
// Software frame: 0x00RRGGBB, row-major, stride == w (pixels).
struct Frame {
  int w = 0, h = 0;
  std::vector<uint32_t> px;
  Ns pts = 0;
  uint32_t generation = 0;
  void resize(int W, int H) { w = W; h = H; px.assign((size_t)W * H, 0u); }
};
inline uint32_t rgb(int r, int g, int b) { return ((uint32_t)(r & 255) << 16) | ((uint32_t)(g & 255) << 8) | (uint32_t)(b & 255); }

// ---- Actions (IF-02) ---------------------------------------------------------------------
enum class ActionId : uint16_t {
  None, NextScene, PrevScene, SelectScene /*value=index*/, PauseAutomation, ResumeAutomation,
  ModeAuto, ModeProgrammed, ModeHybrid, VisualBlackout, LightingBlackout, EmergencyBlackout,
  RestoreBlackout, Brightness /*0..1*/, Speed /*0.25..4*/, Intensity /*0..1*/, ColorHue /*0..1*/,
  TapTempo, SetBpm, ReleaseBpm, SetKey /*value=pc+12*minor*/, ReleaseKey, NextCue, PrevCue,
  TriggerCue /*value=index*/, PublishText /*value=message index*/, ClearText, SelectBank,
  ArmLighting, DisarmLighting, StartRecording, StopRecording, AddMarker, StartBroadcast,
  StopBroadcast, DisableStrobing /*0/1*/, SetTarget /*output index; -1 all*/, ResumeContinue, ResumeNext,
  Count_
};
enum class ActionSource : uint8_t { UI, Keyboard, Midi, Show, System, Remote };
enum class Trigger : uint8_t { Immediate, Beat, Bar, Time };
struct Action {
  ActionId id = ActionId::None;
  ActionSource source = ActionSource::UI;
  int target = -1;                         // logical output index, -1 = all visual outputs
  double value = 0;
  Trigger trigger = Trigger::Immediate;
  Ns when = 0;                             // for Trigger::Time
  uint64_t expectRevision = 0;             // 0 = don't care
  uint64_t correlation = 0;
};
struct ActionResult { bool accepted = false; std::string reason; uint64_t revision = 0; };

} // namespace ss
