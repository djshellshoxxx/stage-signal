// StageSignal record: the segment journal and recovery scan (spec section 12: "Journal completed/active files.
// Recover only complete writes; lost unflushed tail is disclosed", REQ-R04, REQ-R05).
//
// Journal file = text, one record per line:  SSJ1 <crc32 hex8> <json>\n   (crc over the json text)
// A torn last line (crash mid-append) fails its CRC and is ignored and reported, never trusted.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/record/profile.hpp"

namespace ss::record {

// Durable append sink: appendLine must not return ok before the line has been handed to the OS and (where the
// platform allows) flushed to stable storage. The app provides FileJournalSink or its own.
class IJournalSink {
public:
    virtual ~IJournalSink() = default;
    virtual Status appendLine(const std::string& line) = 0;     // line has no trailing newline
};

class FileJournalSink final : public IJournalSink {
public:
    FileJournalSink() = default;
    ~FileJournalSink() override;
    FileJournalSink(const FileJournalSink&) = delete;
    FileJournalSink& operator=(const FileJournalSink&) = delete;
    Status open(const std::string& path);                      // append mode
    Status appendLine(const std::string& line) override;       // fwrite + fflush + fsync/_commit
    void close();
private:
    void* file_ = nullptr;
};

std::uint32_t crc32(const void* data, std::size_t n);

enum class JournalType : std::uint8_t { Begin, Open, Progress, Close, Marker, Gap, End };

struct JournalRecord {
    JournalType type = JournalType::Begin;
    // Begin
    std::string sessionId, profileHash;
    ProfileKind kind = ProfileKind::AudioOnlyWav;
    std::int64_t wallStartMs = 0;
    Ns flushIntervalNs = 0;
    // Open / Progress / Close
    std::uint32_t segment = 0;
    std::string file;
    Ns startNs = 0, endNs = 0, mediaNs = 0;
    std::uint64_t bytes = 0;
    // Marker / Gap / End
    std::string label;
    Ns gapNs = 0;
    bool clean = false;
    std::string reason;
};

class JournalWriter {
public:
    explicit JournalWriter(IJournalSink& sink) : sink_(sink) {}
    Status begin(const std::string& sessionId, const std::string& profileHash, ProfileKind kind, std::int64_t wallStartMs);
    Status segmentOpened(std::uint32_t seg, const std::string& file, Ns startNs);
    Status progress(std::uint32_t seg, std::uint64_t bytes, Ns mediaNs);     // heartbeat after each durable flush
    Status segmentClosed(std::uint32_t seg, std::uint64_t bytes, Ns endNs);
    Status marker(const std::string& label, Ns mediaNs);
    Status gap(Ns mediaNs, Ns lengthNs, const std::string& reason);
    Status end(bool clean, const std::string& reason, Ns mediaNs);
private:
    Status put(const std::string& json);
    IJournalSink& sink_;
};

struct ParsedJournal {
    std::vector<JournalRecord> records;
    std::size_t corruptLines = 0;     // CRC/format failures anywhere
    bool tornTail = false;            // the LAST non-empty line was corrupt (crash while appending)
};
ParsedJournal parseJournal(const std::string& text);

// ---- recovery ------------------------------------------------------------------------------------------------
class IRecoveryFs {
public:
    virtual ~IRecoveryFs() = default;
    virtual bool fileSize(const std::string& name, std::uint64_t& size) = 0;               // false when absent
    virtual Result<std::string> readPrefix(const std::string& name, std::size_t n) = 0;
    virtual Status patch(const std::string& name, std::uint64_t offset, const std::string& bytes) = 0;
    virtual Status truncate(const std::string& name, std::uint64_t newSize) = 0;
};

enum class SegmentState : std::uint8_t {
    Complete,       // journal has Close and the file size matches: untouched
    RecoveredWav,   // active WAV segment: header sizes repaired, trailing partial frame removed
    Unverified,     // active/unclosed non-WAV segment: kept on disk, NOT claimed playable (remux/repair needed)
    Missing,        // journal lists it but the file is gone
    Mismatch        // journal Close size differs from the file: kept, not trusted
};

struct RecoveredSegment {
    std::uint32_t index = 0;
    std::string file;
    SegmentState state = SegmentState::Missing;
    std::uint64_t bytes = 0;
    Ns startNs = 0, endNs = 0;          // endNs for RecoveredWav is derived from the repaired data length
    std::string note;
};

struct LostInterval {
    Ns fromNs = 0;                     // media time after which data is not recoverable
    Ns maxUnflushedNs = 0;             // upper bound of buffered-but-unwritten data (<= flush interval); 0 if clean
    std::string description;
};

struct RecoveryReport {
    bool journalPresent = false;
    bool sessionEndedCleanly = false;
    bool tornTail = false;
    std::size_t corruptLines = 0;
    std::string sessionId;
    ProfileKind kind = ProfileKind::AudioOnlyWav;
    std::vector<RecoveredSegment> segments;
    std::vector<LostInterval> lost;
    Ns recoveredDurationNs = 0;        // sum of Complete + RecoveredWav durations
    std::vector<std::string> notes;    // operator-facing disclosure lines
};

// Reads the journal and repairs only what is provably complete. WAV repair uses the header found on disk
// (see wav.hpp); a segment is never extended, only shortened to whole frames.
RecoveryReport recoverFromJournal(const std::string& journalText, IRecoveryFs& fs);

}  // namespace ss::record
