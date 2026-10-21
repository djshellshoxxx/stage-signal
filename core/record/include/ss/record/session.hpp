// StageSignal record: the recording session state machine (spec section 12 "Session states").
//   Idle -> Preparing -> Recording -> Stopping -> Completed
//   any active state (Preparing, Recording, Stopping) -> Failed (nothing on disk) or Recoverable (completed segments exist)
// Recording is claimed only after prepareChecks passed on the immutable profile (REQ-R06). Show/scene changes never
// reset the session clock. The class does no I/O: the app reports what the writer did and asks what to do next.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/record/disk.hpp"
#include "ss/record/prepare.hpp"
#include "ss/record/profile.hpp"

namespace ss::record {

enum class State : std::uint8_t { Idle, Preparing, Recording, Stopping, Completed, Failed, Recoverable };
enum class StopReason : std::uint8_t { None, User, DiskLow, WriterFault, Shutdown };
const char* stateName(State s);

// Dynamics processing facts that must travel with the recording (REQ-R02). Plain data supplied by the dynamics owner.
struct DynamicsMetadata {
    bool compressorEnabled = true, limiterEnabled = true, bypassed = false;
    float compressorThresholdDb = -12.f, compressorRatio = 3.f, limiterCeilingDb = -1.f;
    std::uint64_t compressorActiveBlocks = 0, limiterActiveBlocks = 0;
    bool inputClipWarningSeen = false;
    std::uint32_t inputClipEvents = 0;
    Ns processingDelayNs = 0;
};

struct Marker {
    std::uint32_t id = 0;
    Ns sessionNs = 0;                  // media-clock time since the session started
    std::string label;                 // sanitised, <= 128 code points
    std::uint32_t segment = 0;         // 0 when the time falls in no known segment yet
    Ns offsetInSegmentNs = 0;
};

struct SessionSegment {
    std::uint32_t index = 0;
    std::string file;
    Ns startNs = 0, endNs = 0;
    std::uint64_t bytes = 0;
    bool closed = false;
};

struct LossRecord {                    // interval of the show that is NOT in the recording
    Ns fromNs = 0;
    Ns lengthNs = 0;
    bool lengthKnown = false;
    std::string reason;
};

struct SessionSummary {
    std::string sessionId, profileHash;
    State state = State::Idle;
    StopReason stopReason = StopReason::None;
    bool endedEarly = false;           // stopped by disk-low or a fault rather than by the user
    std::vector<SessionSegment> segments;
    std::vector<Marker> markers;
    std::vector<LossRecord> losses;
    Ns durationNs = 0;
    std::uint64_t totalBytes = 0;
    DiskLevel lastDisk = DiskLevel::Ok;
    bool hasError = false;
    Error lastError;
    std::string encoderName;           // empty for audio-only
    std::vector<PrepareIssue> prepareIssues;
};

class RecordSession {
public:
    explicit RecordSession(const IClock& clock) : clock_(clock) {}

    State state() const { return summary_.state; }
    const SessionSummary& summary() const { return summary_; }

    // Idle (or a finished session) -> Preparing. The profile is captured by value and cannot change afterwards.
    Status beginPrepare(const RecordProfile& profile, const std::string& sessionId);
    // Preparing -> Recording when the checks pass; otherwise Failed with actionable issues in the report.
    PrepareReport completePrepare(const PrepareInputs& inputs);

    // Writer feedback while Recording/Stopping.
    void onSegmentOpened(std::uint32_t index, const std::string& file, Ns startNs);
    void onSegmentClosed(std::uint32_t index, std::uint64_t bytes, Ns endNs);
    void onBytesWritten(std::uint64_t totalBytes);
    // Periodic: pass the free space of the output volume. At StopNow the session moves to Stopping by itself
    // (StopReason::DiskLow); the caller sees state()==Stopping and asks the writer to stop gracefully.
    DiskStatus checkDisk(std::uint64_t freeBytes);

    Status requestStop(StopReason reason = StopReason::User);       // Recording -> Stopping (Stopping: no-op ok)
    void onWriterStopped(bool clean);                                // Stopping -> Completed (clean) | Recoverable/Failed
    // Writer or device fault in any active state. lengthKnown=false when the lost interval cannot be measured.
    void onWriterFault(const Error& e, bool lengthKnown = false, Ns lostLengthNs = 0);
    void noteLoss(Ns fromNs, Ns lengthNs, bool lengthKnown, const std::string& reason);   // dropped input, capture gaps
    void noteShowEvent() { ++showEvents_; }                          // scene/cue change: never touches the session clock
    std::uint32_t showEvents() const { return showEvents_; }

    Result<Marker> addMarker(const std::string& label);              // Recording only; stamped with the session clock
    Result<Marker> addMarkerAt(const std::string& label, Ns sessionNs);   // explicit media time (e.g. from a MIDI timestamp)
    void reset();                                                    // terminal state -> Idle

    Ns elapsedNs() const;                                            // frozen when the session stops
    const RecordProfile* profile() const { return profile_ ? &*profile_ : nullptr; }
    // Sidecar JSON describing the session: profile hash, segments, markers, losses, dynamics settings (REQ-R02).
    std::string metadataJson(const DynamicsMetadata& dyn) const;

private:
    void fault(const Error& e);
    void finishClock();

    const IClock& clock_;
    SessionSummary summary_;
    std::optional<RecordProfile> profile_;
    Ns startMono_ = 0, endMono_ = -1;
    std::uint32_t nextMarker_ = 1, showEvents_ = 0;
    std::unique_ptr<DiskMonitor> disk_;
};

// Marker exports. CSV: id,session_ms,segment,offset_ms,label. FFMETADATA1 chapters for a later offline mux.
std::string markersCsv(const std::vector<Marker>& markers);
std::string markersFfmetadata(const std::vector<Marker>& markers, Ns totalDurationNs);

}  // namespace ss::record
