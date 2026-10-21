#include "ss/record/session.hpp"
#include <algorithm>
#include <cstdio>
#include "nlohmann/json.hpp"
#include "ss/base/utf8.hpp"

namespace ss::record {
using json = nlohmann::json;

const char* stateName(State s) {
    switch (s) {
        case State::Idle: return "Idle";
        case State::Preparing: return "Preparing";
        case State::Recording: return "Recording";
        case State::Stopping: return "Stopping";
        case State::Completed: return "Completed";
        case State::Failed: return "Failed";
        case State::Recoverable: return "Recoverable";
    }
    return "?";
}

namespace {
bool active(State s) { return s == State::Preparing || s == State::Recording || s == State::Stopping; }
bool terminal(State s) { return s == State::Completed || s == State::Failed || s == State::Recoverable; }
ErrorCategory categoryFor(IssueCode c) {
    switch (c) {
        case IssueCode::PathMissing: case IssueCode::PathNotWritable: return ErrorCategory::PermissionDenied;
        case IssueCode::DiskUnknown: case IssueCode::DiskTooLow: case IssueCode::DiskLowWarning: return ErrorCategory::DiskFull;
        case IssueCode::FfmpegMissing: case IssueCode::EncoderUnavailable: case IssueCode::EncoderFallback: case IssueCode::MuxerMissing:
            return ErrorCategory::EncoderUnavailable;
        case IssueCode::AudioSourceMissing: case IssueCode::VideoSourceMissing: return ErrorCategory::DeviceLost;
        case IssueCode::AudioFormatMismatch: case IssueCode::VideoFormatMismatch: case IssueCode::NotPreparing: return ErrorCategory::InvalidArgument;
    }
    return ErrorCategory::InternalFault;
}
}  // namespace

Status RecordSession::beginPrepare(const RecordProfile& profile, const std::string& sessionId) {
    if (active(summary_.state))
        return makeError(ErrorCategory::InvalidArgument, std::string("a recording is already ") + stateName(summary_.state));
    if (sessionId.size() != 8)
        return makeError(ErrorCategory::InvalidArgument, "invalid session id");
    summary_ = SessionSummary{};
    summary_.sessionId = sessionId;
    summary_.profileHash = profile.hash();
    summary_.state = State::Preparing;
    profile_ = profile;
    disk_.reset();
    startMono_ = 0; endMono_ = -1; nextMarker_ = 1; showEvents_ = 0;
    return okStatus();
}

PrepareReport RecordSession::completePrepare(const PrepareInputs& in) {
    if (summary_.state != State::Preparing || !profile_) {
        PrepareReport r; r.ok = false;
        r.issues.push_back({IssueCode::NotPreparing, true, "Recording is not in the Preparing state.", "Start the recording again."});
        return r;
    }
    PrepareReport rep = prepareChecks(*profile_, in);
    summary_.prepareIssues = rep.issues;
    if (!rep.ok) {
        summary_.state = State::Failed;
        summary_.hasError = true;
        for (const auto& i : rep.issues)
            if (i.blocking) { summary_.lastError = makeError(categoryFor(i.code), i.message, Recoverability::OperatorAction); break; }
        return rep;
    }
    if (rep.encoder) summary_.encoderName = rep.encoder->name;
    disk_ = std::make_unique<DiskMonitor>(rep.nominal.bytesPerSec, rep.nominal.relUncertainty);
    startMono_ = clock_.nowMonotonic();
    summary_.state = State::Recording;
    return rep;
}

Ns RecordSession::elapsedNs() const {
    if (summary_.state == State::Idle || summary_.state == State::Preparing) return 0;
    const Ns end = endMono_ >= 0 ? endMono_ : clock_.nowMonotonic();
    return std::max<Ns>(0, end - startMono_);
}
void RecordSession::finishClock() {
    if (endMono_ < 0 && (summary_.state == State::Recording || summary_.state == State::Stopping)) endMono_ = clock_.nowMonotonic();
}

void RecordSession::onSegmentOpened(std::uint32_t index, const std::string& file, Ns startNs) {
    if (summary_.state != State::Recording && summary_.state != State::Stopping) return;
    SessionSegment s; s.index = index; s.file = file; s.startNs = startNs;
    summary_.segments.push_back(s);
}
void RecordSession::onSegmentClosed(std::uint32_t index, std::uint64_t bytes, Ns endNs) {
    for (auto& s : summary_.segments)
        if (s.index == index) { s.closed = true; s.bytes = bytes; s.endNs = endNs; }
}
void RecordSession::onBytesWritten(std::uint64_t total) {
    if (summary_.state != State::Recording && summary_.state != State::Stopping) return;
    summary_.totalBytes = total;
    if (disk_) disk_->onBytesWritten(elapsedNs(), total);
}

DiskStatus RecordSession::checkDisk(std::uint64_t freeBytes) {
    DiskStatus st;
    if (!disk_ || (summary_.state != State::Recording && summary_.state != State::Stopping)) return st;
    st = disk_->update(freeBytes);
    summary_.lastDisk = st.level;
    if (st.level == DiskLevel::StopNow && summary_.state == State::Recording) (void)requestStop(StopReason::DiskLow);
    return st;
}

Status RecordSession::requestStop(StopReason reason) {
    if (summary_.state == State::Stopping) return okStatus();
    if (summary_.state != State::Recording)
        return makeError(ErrorCategory::InvalidArgument, std::string("cannot stop while ") + stateName(summary_.state));
    finishClock();
    summary_.state = State::Stopping;
    summary_.stopReason = reason;
    return okStatus();
}

void RecordSession::fault(const Error& e) {
    finishClock();
    summary_.hasError = true;
    summary_.lastError = e;
    if (summary_.stopReason == StopReason::None || summary_.stopReason == StopReason::User) summary_.stopReason = StopReason::WriterFault;
    summary_.endedEarly = true;
    summary_.durationNs = elapsedNs();
    summary_.state = summary_.segments.empty() ? State::Failed : State::Recoverable;
}

void RecordSession::onWriterStopped(bool clean) {
    if (summary_.state != State::Stopping) return;
    if (!clean) { fault(makeError(ErrorCategory::InternalFault, "the writer did not close cleanly", Recoverability::OperatorAction)); return; }
    summary_.durationNs = elapsedNs();
    summary_.endedEarly = summary_.stopReason == StopReason::DiskLow || summary_.stopReason == StopReason::WriterFault;
    summary_.state = State::Completed;
}

void RecordSession::onWriterFault(const Error& e, bool lengthKnown, Ns lostLen) {
    if (!active(summary_.state)) return;
    if (summary_.state != State::Preparing) {
        LossRecord l; l.fromNs = elapsedNs(); l.lengthKnown = lengthKnown; l.lengthNs = lostLen; l.reason = e.detail;
        summary_.losses.push_back(l);
    }
    fault(e);
}

void RecordSession::noteLoss(Ns fromNs, Ns lengthNs, bool lengthKnown, const std::string& reason) {
    if (summary_.state != State::Recording && summary_.state != State::Stopping) return;
    summary_.losses.push_back({fromNs, lengthNs, lengthKnown, reason});
}

Result<Marker> RecordSession::addMarker(const std::string& label) {
    if (summary_.state != State::Recording)
        return makeError(ErrorCategory::InvalidArgument, "markers can only be added while recording");
    if (summary_.markers.size() >= Limits::kMaxMarkers)
        return makeError(ErrorCategory::ResourceExhausted, "marker limit reached for this recording");
    return addMarkerAt(label, elapsedNs());
}

Result<Marker> RecordSession::addMarkerAt(const std::string& label, Ns sessionNs) {
    if (summary_.state != State::Recording && summary_.state != State::Stopping)
        return makeError(ErrorCategory::InvalidArgument, "markers can only be added while recording");
    if (summary_.markers.size() >= Limits::kMaxMarkers)
        return makeError(ErrorCategory::ResourceExhausted, "marker limit reached for this recording");
    if (!utf8::isValid(label)) return makeError(ErrorCategory::InvalidArgument, "marker label is not valid UTF-8");
    Marker m;
    m.id = nextMarker_++;
    m.sessionNs = std::max<Ns>(0, sessionNs);
    std::string clean;
    long cps = 0;
    for (std::size_t i = 0; i < label.size();) {
        const unsigned char c = static_cast<unsigned char>(label[i]);
        const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (static_cast<std::size_t>(cps) >= Limits::kMaxMarkerLabelCodePoints) break;      // truncate: a live key press must not fail
        if (c < 0x20 || c == 0x7F) clean.push_back(' ');
        else clean.append(label, i, len);
        i += len; ++cps;
    }
    m.label = clean;
    for (const auto& s : summary_.segments)
        if (s.startNs <= m.sessionNs && (!s.closed || m.sessionNs < s.endNs || s.endNs == s.startNs)) { m.segment = s.index; m.offsetInSegmentNs = m.sessionNs - s.startNs; }
    summary_.markers.push_back(m);
    return m;
}

void RecordSession::reset() {
    if (!terminal(summary_.state) && summary_.state != State::Idle) return;
    summary_ = SessionSummary{};
    profile_.reset();
    disk_.reset();
}

std::string RecordSession::metadataJson(const DynamicsMetadata& d) const {
    json j;
    j["schema"] = 1;
    j["sessionId"] = summary_.sessionId;
    j["profileHash"] = summary_.profileHash;
    j["state"] = stateName(summary_.state);
    j["stopReason"] = static_cast<int>(summary_.stopReason);
    j["endedEarly"] = summary_.endedEarly;
    j["durationMs"] = summary_.durationNs / kNsPerMs;
    j["totalBytes"] = summary_.totalBytes;
    j["encoder"] = summary_.encoderName;
    if (profile_) {
        const auto& s = profile_->spec();
        j["kind"] = s.kind == ProfileKind::AudioOnlyWav ? "audio-wav" : "video-matroska";
        j["audio"] = {{"rateHz", s.audio.rateHz}, {"channels", s.audio.channels}, {"bits", 24}};
        j["audioDelayCompensationNs"] = s.audioDelayCompensationNs;
        if (profile_->hasVideo()) j["video"] = {{"width", s.video.width}, {"height", s.video.height}, {"fpsNum", s.video.fpsNum}, {"fpsDen", s.video.fpsDen}};
    }
    j["dynamics"] = {{"compressorEnabled", d.compressorEnabled}, {"limiterEnabled", d.limiterEnabled}, {"bypassed", d.bypassed},
                     {"compressorThresholdDb", d.compressorThresholdDb}, {"compressorRatio", d.compressorRatio},
                     {"limiterCeilingDb", d.limiterCeilingDb}, {"compressorActiveBlocks", d.compressorActiveBlocks},
                     {"limiterActiveBlocks", d.limiterActiveBlocks}, {"inputClipWarningSeen", d.inputClipWarningSeen},
                     {"inputClipEvents", d.inputClipEvents}, {"processingDelayNs", d.processingDelayNs}};
    j["segments"] = json::array();
    for (const auto& s : summary_.segments)
        j["segments"].push_back({{"index", s.index}, {"file", s.file}, {"startNs", s.startNs}, {"endNs", s.endNs}, {"bytes", s.bytes}, {"closed", s.closed}});
    j["markers"] = json::array();
    for (const auto& m : summary_.markers)
        j["markers"].push_back({{"id", m.id}, {"sessionNs", m.sessionNs}, {"label", m.label}, {"segment", m.segment}, {"offsetNs", m.offsetInSegmentNs}});
    j["losses"] = json::array();
    for (const auto& l : summary_.losses)
        j["losses"].push_back({{"fromNs", l.fromNs}, {"lengthNs", l.lengthNs}, {"lengthKnown", l.lengthKnown}, {"reason", l.reason}});
    return j.dump(2);
}

// ---- marker exports ----------------------------------------------------------------------------------------------
std::string markersCsv(const std::vector<Marker>& markers) {
    std::string out = "id,session_ms,segment,offset_ms,label\n";
    for (const auto& m : markers) {
        std::string label = m.label;
        if (!label.empty() && (label[0] == '=' || label[0] == '+' || label[0] == '-' || label[0] == '@')) label.insert(label.begin(), '\'');   // spreadsheet formula guard
        std::string q = "\"";
        for (char c : label) { if (c == '"') q.push_back('"'); q.push_back(c); }
        q.push_back('"');
        out += std::to_string(m.id) + "," + std::to_string(m.sessionNs / kNsPerMs) + "," + std::to_string(m.segment) + "," +
               std::to_string(m.offsetInSegmentNs / kNsPerMs) + "," + q + "\n";
    }
    return out;
}

std::string markersFfmetadata(const std::vector<Marker>& markers, Ns totalDurationNs) {
    std::string out = ";FFMETADATA1\n";
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const Ns start = markers[i].sessionNs / kNsPerMs;
        Ns end = (i + 1 < markers.size() ? markers[i + 1].sessionNs : totalDurationNs) / kNsPerMs;
        if (end <= start) end = start + 1;
        out += "[CHAPTER]\nTIMEBASE=1/1000\nSTART=" + std::to_string(start) + "\nEND=" + std::to_string(end) + "\ntitle=";
        for (char c : markers[i].label) {
            if (c == '=' || c == ';' || c == '#' || c == '\\') out.push_back('\\');
            if (c == '\n') { out += "\\\n"; continue; }
            out.push_back(c);
        }
        out.push_back('\n');
    }
    return out;
}

}  // namespace ss::record
