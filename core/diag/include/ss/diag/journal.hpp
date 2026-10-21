// StageSignal diag: bounded event journal with marker API (REQ-X05, spec section 17).
// Every event carries a monotonic and a wall-clock reference (IClock), a sequence number, and sanitised detail text
// (redacted, single line, <= 256 bytes). The ring never grows: when full the oldest event is overwritten and counted.
// Not thread-safe by itself; the owner (AppController) serialises access or uses one journal per thread.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/time.hpp"
#include "ss/model/show_model.hpp"

namespace ss::diag {

enum class EventKind : std::uint8_t {
    Scene, Cue, Mode, Text, Tempo, Key, Marker, Device, Adapt, Recording, Broadcast, Discontinuity, Error, Note, Count
};

const char* kindName(EventKind k);        // stable machine name: "scene","cue","mode","text","tempo","key","marker","device",
                                          // "adapt","rec","cast","gap","error","note" (matches model::JournalEntry kinds)
const char* kindLabel(EventKind k);       // human label for reports
bool parseKindName(const std::string& name, EventKind& out);

struct Event {
    std::uint64_t seq = 0;                // 1-based, strictly increasing, survives ring overwrite
    Ns monoNs = 0;
    std::int64_t wallUnixMs = 0;
    EventKind kind = EventKind::Note;
    std::uint32_t id = 0;                 // scene/cue/text id when relevant
    std::string detail;                   // sanitised
};

constexpr std::size_t kJournalDetailMaxBytes = 256;
constexpr std::size_t kJournalMinCapacity = 16, kJournalMaxCapacity = 100000;

class EventJournal {
public:
    explicit EventJournal(const IClock& clock, std::size_t capacity = 4096);

    std::uint64_t append(EventKind kind, const std::string& detail, std::uint32_t id = 0);
    // Marker API: an operator or MIDI/hotkey marker. Empty label becomes "Marker <n>". Returns the event sequence.
    std::uint64_t addMarker(const std::string& label);
    // Explicit timestamps (replay, imports, tests). Same sanitising as append().
    std::uint64_t appendAt(Ns monoNs, std::int64_t wallUnixMs, EventKind kind, const std::string& detail, std::uint32_t id = 0);

    std::vector<Event> snapshot() const;                 // oldest first
    std::vector<Event> markers() const;                  // oldest first
    std::size_t size() const { return count_; }
    std::size_t capacity() const { return buf_.size(); }
    std::uint64_t totalAppended() const { return nextSeq_ - 1; }
    std::uint64_t overwritten() const { return overwritten_; }
    std::uint64_t markerCount() const { return markerCount_; }
    void clear();

    // For the show document's embedded journal (model::JournalEntry, section 17).
    std::vector<model::JournalEntry> toModelEntries(std::size_t maxEntries = 1000) const;

private:
    const IClock& clock_;
    std::vector<Event> buf_;
    std::size_t head_ = 0, count_ = 0;       // head_ = next write slot
    std::uint64_t nextSeq_ = 1, overwritten_ = 0, markerCount_ = 0;
};

}  // namespace ss::diag
