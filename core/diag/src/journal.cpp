#include "ss/diag/journal.hpp"

#include <algorithm>
#include "ss/security/redact.hpp"

namespace ss::diag {
namespace {
struct KindInfo { const char* name; const char* label; };
const KindInfo kKinds[] = {
    {"scene", "Scene"}, {"cue", "Cue"}, {"mode", "Mode"}, {"text", "Text"}, {"tempo", "Tempo"}, {"key", "Key"}, {"marker", "Marker"},
    {"device", "Device"}, {"adapt", "Adaptation"}, {"rec", "Recording"}, {"cast", "Broadcast"}, {"gap", "Discontinuity"},
    {"error", "Error"}, {"note", "Note"}};
static_assert(sizeof(kKinds) / sizeof(kKinds[0]) == static_cast<std::size_t>(EventKind::Count), "kind table out of sync");
}  // namespace

const char* kindName(EventKind k) { return static_cast<std::size_t>(k) < static_cast<std::size_t>(EventKind::Count) ? kKinds[static_cast<std::size_t>(k)].name : "note"; }
const char* kindLabel(EventKind k) { return static_cast<std::size_t>(k) < static_cast<std::size_t>(EventKind::Count) ? kKinds[static_cast<std::size_t>(k)].label : "Note"; }

bool parseKindName(const std::string& name, EventKind& out) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(EventKind::Count); ++i)
        if (name == kKinds[i].name) { out = static_cast<EventKind>(i); return true; }
    return false;
}

EventJournal::EventJournal(const IClock& clock, std::size_t capacity)
    : clock_(clock), buf_(std::min(std::max(capacity, kJournalMinCapacity), kJournalMaxCapacity)) {}

std::uint64_t EventJournal::appendAt(Ns mono, std::int64_t wallMs, EventKind kind, const std::string& detail, std::uint32_t id) {
    Event& e = buf_[head_];
    if (count_ == buf_.size()) ++overwritten_; else ++count_;
    e.seq = nextSeq_++;
    e.monoNs = mono;
    e.wallUnixMs = wallMs;
    e.kind = kind;
    e.id = id;
    e.detail = security::sanitizeForLog(detail, kJournalDetailMaxBytes);
    head_ = (head_ + 1) % buf_.size();
    if (kind == EventKind::Marker) ++markerCount_;
    return e.seq;
}

std::uint64_t EventJournal::append(EventKind kind, const std::string& detail, std::uint32_t id) {
    return appendAt(clock_.nowMonotonic(), clock_.wallUnixNs() / kNsPerMs, kind, detail, id);
}

std::uint64_t EventJournal::addMarker(const std::string& label) {
    std::string l = security::sanitizeForLog(label, 120);
    // trim spaces produced by control-character replacement
    const auto b = l.find_first_not_of(' ');
    l = b == std::string::npos ? std::string() : l.substr(b, l.find_last_not_of(' ') - b + 1);
    if (l.empty()) l = "Marker " + std::to_string(markerCount_ + 1);
    return append(EventKind::Marker, l);
}

std::vector<Event> EventJournal::snapshot() const {
    std::vector<Event> out;
    out.reserve(count_);
    const std::size_t start = count_ == buf_.size() ? head_ : 0;
    for (std::size_t i = 0; i < count_; ++i) out.push_back(buf_[(start + i) % buf_.size()]);
    return out;
}

std::vector<Event> EventJournal::markers() const {
    std::vector<Event> out;
    for (auto& e : snapshot()) if (e.kind == EventKind::Marker) out.push_back(std::move(e));
    return out;
}

void EventJournal::clear() { head_ = 0; count_ = 0; }

std::vector<model::JournalEntry> EventJournal::toModelEntries(std::size_t maxEntries) const {
    const auto all = snapshot();
    const std::size_t from = all.size() > maxEntries ? all.size() - maxEntries : 0;
    std::vector<model::JournalEntry> out;
    for (std::size_t i = from; i < all.size(); ++i)
        out.push_back({all[i].monoNs, all[i].wallUnixMs, kindName(all[i].kind), all[i].detail});
    return out;
}

}  // namespace ss::diag
