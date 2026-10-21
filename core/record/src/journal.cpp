#include "ss/record/journal.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include "nlohmann/json.hpp"
#include "ss/record/wav.hpp"

namespace ss::record {
using json = nlohmann::json;

std::uint32_t crc32(const void* data, std::size_t n) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {                       // benign race: identical values are written
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---- FileJournalSink ---------------------------------------------------------------------------------------------
FileJournalSink::~FileJournalSink() { close(); }
Status FileJournalSink::open(const std::string& path) {
    close();
    file_ = std::fopen(path.c_str(), "ab");
    if (!file_) return makeError(ErrorCategory::PermissionDenied, "cannot open the session journal", Recoverability::OperatorAction);
    return okStatus();
}
Status FileJournalSink::appendLine(const std::string& line) {
    FILE* f = static_cast<FILE*>(file_);
    if (!f) return makeError(ErrorCategory::InternalFault, "journal is not open");
    const std::string l = line + "\n";
    if (std::fwrite(l.data(), 1, l.size(), f) != l.size() || std::fflush(f) != 0)
        return makeError(ErrorCategory::DiskFull, "journal write failed", Recoverability::OperatorAction);
#ifdef _WIN32
    (void)_commit(_fileno(f));
#else
    (void)fsync(fileno(f));
#endif
    return okStatus();
}
void FileJournalSink::close() {
    if (file_) { std::fclose(static_cast<FILE*>(file_)); file_ = nullptr; }
}

// ---- writer ------------------------------------------------------------------------------------------------------
Status JournalWriter::put(const std::string& js) {
    char crc[16];
    std::snprintf(crc, sizeof crc, "%08x", crc32(js.data(), js.size()));
    return sink_.appendLine(std::string("SSJ1 ") + crc + " " + js);
}
Status JournalWriter::begin(const std::string& id, const std::string& hash, ProfileKind kind, std::int64_t wallMs) {
    json j = {{"t", "begin"}, {"id", id}, {"ph", hash}, {"kind", static_cast<int>(kind)}, {"wall", wallMs},
              {"flush", Limits::kFlushIntervalNs}};
    return put(j.dump());
}
Status JournalWriter::segmentOpened(std::uint32_t seg, const std::string& file, Ns startNs) {
    return put(json{{"t", "open"}, {"seg", seg}, {"file", file}, {"start", startNs}}.dump());
}
Status JournalWriter::progress(std::uint32_t seg, std::uint64_t bytes, Ns mediaNs) {
    return put(json{{"t", "prog"}, {"seg", seg}, {"bytes", bytes}, {"media", mediaNs}}.dump());
}
Status JournalWriter::segmentClosed(std::uint32_t seg, std::uint64_t bytes, Ns endNs) {
    return put(json{{"t", "close"}, {"seg", seg}, {"bytes", bytes}, {"end", endNs}}.dump());
}
Status JournalWriter::marker(const std::string& label, Ns mediaNs) {
    return put(json{{"t", "mark"}, {"label", label}, {"media", mediaNs}}.dump());
}
Status JournalWriter::gap(Ns mediaNs, Ns lengthNs, const std::string& reason) {
    return put(json{{"t", "gap"}, {"media", mediaNs}, {"len", lengthNs}, {"reason", reason}}.dump());
}
Status JournalWriter::end(bool clean, const std::string& reason, Ns mediaNs) {
    return put(json{{"t", "end"}, {"clean", clean}, {"reason", reason}, {"media", mediaNs}}.dump());
}

// ---- parser ------------------------------------------------------------------------------------------------------
namespace {
template <class T> T num(const json& j, const char* k, T def = T{}) {
    auto it = j.find(k);
    return (it != j.end() && it->is_number()) ? it->get<T>() : def;
}
std::string str(const json& j, const char* k) {
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}
}  // namespace

ParsedJournal parseJournal(const std::string& text) {
    ParsedJournal out;
    std::istringstream in(text);
    std::string line;
    bool lastBad = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        lastBad = true;
        // "SSJ1 " + 8 hex + " " + json
        if (line.size() < 15 || line.compare(0, 5, "SSJ1 ") != 0 || line[13] != ' ') { ++out.corruptLines; continue; }
        char* endp = nullptr;
        const unsigned long want = std::strtoul(line.substr(5, 8).c_str(), &endp, 16);
        const std::string body = line.substr(14);
        if (*endp != '\0' || static_cast<std::uint32_t>(want) != crc32(body.data(), body.size())) { ++out.corruptLines; continue; }
        json j = json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { ++out.corruptLines; continue; }
        JournalRecord r;
        const std::string t = str(j, "t");
        if (t == "begin") {
            r.type = JournalType::Begin; r.sessionId = str(j, "id"); r.profileHash = str(j, "ph");
            r.kind = num<int>(j, "kind") == 1 ? ProfileKind::VideoMatroska : ProfileKind::AudioOnlyWav;
            r.wallStartMs = num<std::int64_t>(j, "wall"); r.flushIntervalNs = num<std::int64_t>(j, "flush");
        } else if (t == "open") {
            r.type = JournalType::Open; r.segment = num<std::uint32_t>(j, "seg"); r.file = str(j, "file"); r.startNs = num<std::int64_t>(j, "start");
        } else if (t == "prog") {
            r.type = JournalType::Progress; r.segment = num<std::uint32_t>(j, "seg"); r.bytes = num<std::uint64_t>(j, "bytes"); r.mediaNs = num<std::int64_t>(j, "media");
        } else if (t == "close") {
            r.type = JournalType::Close; r.segment = num<std::uint32_t>(j, "seg"); r.bytes = num<std::uint64_t>(j, "bytes"); r.endNs = num<std::int64_t>(j, "end");
        } else if (t == "mark") {
            r.type = JournalType::Marker; r.label = str(j, "label"); r.mediaNs = num<std::int64_t>(j, "media");
        } else if (t == "gap") {
            r.type = JournalType::Gap; r.mediaNs = num<std::int64_t>(j, "media"); r.gapNs = num<std::int64_t>(j, "len"); r.reason = str(j, "reason");
        } else if (t == "end") {
            r.type = JournalType::End; r.clean = j.value("clean", false); r.reason = str(j, "reason"); r.mediaNs = num<std::int64_t>(j, "media");
        } else { ++out.corruptLines; continue; }
        out.records.push_back(std::move(r));
        lastBad = false;
    }
    out.tornTail = lastBad;
    return out;
}

// ---- recovery ----------------------------------------------------------------------------------------------------
RecoveryReport recoverFromJournal(const std::string& text, IRecoveryFs& fs) {
    RecoveryReport rep;
    const ParsedJournal pj = parseJournal(text);
    rep.journalPresent = !pj.records.empty();
    rep.corruptLines = pj.corruptLines;
    rep.tornTail = pj.tornTail;
    if (!rep.journalPresent) { rep.notes.push_back("No readable journal: nothing can be verified or recovered automatically."); return rep; }

    struct Seg { RecoveredSegment r; bool opened = false, closed = false; std::uint64_t closeBytes = 0, progBytes = 0; Ns progMedia = 0; };
    std::map<std::uint32_t, Seg> segs;
    Ns flushNs = Limits::kFlushIntervalNs;
    bool endSeen = false, endClean = false;
    for (const auto& r : pj.records) {
        switch (r.type) {
            case JournalType::Begin: rep.sessionId = r.sessionId; rep.kind = r.kind; if (r.flushIntervalNs > 0) flushNs = r.flushIntervalNs; break;
            case JournalType::Open: { auto& s = segs[r.segment]; s.r.index = r.segment; s.r.file = r.file; s.r.startNs = r.startNs; s.opened = true; break; }
            case JournalType::Progress: { auto& s = segs[r.segment]; s.progBytes = r.bytes; s.progMedia = r.mediaNs; break; }
            case JournalType::Close: { auto& s = segs[r.segment]; s.closed = true; s.closeBytes = r.bytes; s.r.endNs = r.endNs; break; }
            case JournalType::End: endSeen = true; endClean = r.clean; break;
            default: break;
        }
    }
    rep.sessionEndedCleanly = endSeen && endClean;

    for (auto& kv : segs) {
        Seg& s = kv.second;
        if (!s.opened) continue;
        RecoveredSegment& r = s.r;
        std::uint64_t size = 0;
        const bool exists = fs.fileSize(r.file, size);
        if (!exists) { r.state = SegmentState::Missing; r.note = "file listed in the journal is missing"; rep.segments.push_back(r); continue; }
        r.bytes = size;
        if (s.closed) {
            if (size == s.closeBytes) { r.state = SegmentState::Complete; rep.recoveredDurationNs += r.endNs - r.startNs; }
            else { r.state = SegmentState::Mismatch; r.note = "file size differs from the journal; kept but not trusted"; }
            rep.segments.push_back(r);
            continue;
        }
        // Unclosed (active at the crash)
        if (rep.kind == ProfileKind::AudioOnlyWav) {
            auto hdr = fs.readPrefix(r.file, 256);
            Result<WavHeaderInfo> info = hdr.ok() ? parseWavHeader(hdr.value()) : Result<WavHeaderInfo>(hdr.error());
            if (info.ok()) {
                auto plan = planWavRepair(info.value(), size);
                if (plan.ok()) {
                    bool good = true;
                    if (plan.value().droppedPartialFrame) good = fs.truncate(r.file, plan.value().truncateTo).ok();
                    if (good && (plan.value().headerWasWrong || plan.value().droppedPartialFrame))
                        good = fs.patch(r.file, 0, makeWavHeader(info.value().rateHz, info.value().channels, plan.value().dataBytes)).ok();
                    if (good) {
                        r.state = SegmentState::RecoveredWav;
                        r.bytes = plan.value().truncateTo;
                        r.endNs = r.startNs + samplesToNs(plan.value().dataBytes / info.value().blockAlign, info.value().rateHz);
                        rep.recoveredDurationNs += r.endNs - r.startNs;
                        rep.lost.push_back({r.endNs, flushNs, "unflushed audio at the end of the recording is lost (at most the last flush interval)"});
                        r.note = plan.value().droppedPartialFrame ? "header repaired, trailing partial frame removed" : "header repaired";
                        rep.segments.push_back(r);
                        continue;
                    }
                }
            }
        }
        r.state = SegmentState::Unverified;
        r.endNs = s.progMedia;
        r.note = "recording was interrupted; this file is kept but is not guaranteed playable (remux or repair it with an external tool)";
        rep.lost.push_back({s.progMedia, flushNs, "media after the last journaled flush is not verified"});
        rep.segments.push_back(r);
    }
    if (!endSeen) rep.notes.push_back("The session did not end cleanly (power loss, crash or forced exit). Anything after the crash was never recorded.");
    else if (!endClean) rep.notes.push_back("The session ended after a fault; see the segment list for what is complete.");
    if (rep.tornTail) rep.notes.push_back("The last journal line was incomplete and was ignored.");
    if (rep.corruptLines > (rep.tornTail ? 1u : 0u)) rep.notes.push_back("Some journal lines failed their checksum and were ignored.");
    return rep;
}

}  // namespace ss::record
