#include "recording/journal.h"
#include "nlohmann/json.hpp"
namespace ss {
using nlohmann::json;
static json segJson(const char* ev, const SegmentInfo& s) {
  json j; j["ev"] = ev; j["index"] = s.index; j["path"] = s.path; j["kind"] = s.kind;
  j["start_ns"] = s.startNs; j["end_ns"] = s.endNs; j["first_sample"] = s.firstSample; j["audio_frames"] = s.audioFrames;
  j["video_frames"] = s.videoFrames; j["video_dups"] = s.videoDups; j["bytes"] = s.bytes; return j;
}
Status SegmentJournal::open(IFileSystem& fs, const std::string& path) {
  path_ = path; return fs.open(path, IFileSystem::Mode::Append, sink_);
}
Status SegmentJournal::line(const std::string& s) {
  if (!sink_) return Status::Fail(Err::InternalFault, "journal not open");
  std::string l = s + "\n";
  Status st = sink_->write(l.data(), l.size());
  if (!st.ok()) return st;
  return sink_->flush();
}
Status SegmentJournal::session(const std::string& id, const std::string& show, int64_t ms, const std::string& container) {
  json j; j["ev"] = "session"; j["id"] = id; j["show"] = show; j["start_unix_ms"] = ms; j["container"] = container; return line(j.dump());
}
Status SegmentJournal::segmentOpen(const SegmentInfo& s) { return line(segJson("segment_open", s).dump()); }
Status SegmentJournal::segmentClose(const SegmentInfo& s) { return line(segJson("segment_close", s).dump()); }
Status SegmentJournal::note(const std::string& ev, const std::string& d) { json j; j["ev"] = "note"; j["what"] = ev; j["detail"] = d; return line(j.dump()); }
Status SegmentJournal::close() { if (!sink_) return Status::Ok(); Status s = sink_->close(); sink_.reset(); return s; }

std::vector<SegmentJournal::Entry> SegmentJournal::parse(const rec::Bytes& b) {
  std::vector<Entry> out;
  size_t pos = 0;
  while (pos < b.size()) {
    size_t e = pos; while (e < b.size() && b[e] != '\n') ++e;
    if (e >= b.size()) break;                       // torn last line: ignored
    std::string ln((const char*)b.data() + pos, e - pos); pos = e + 1;
    json j = json::parse(ln, nullptr, false);
    if (!j.is_object()) continue;
    std::string ev = j.value("ev", "");
    if (ev != "segment_open" && ev != "segment_close") continue;
    SegmentInfo s; s.index = j.value("index", 0); s.path = j.value("path", ""); s.kind = j.value("kind", "");
    s.startNs = j.value("start_ns", (int64_t)0); s.endNs = j.value("end_ns", (int64_t)0);
    s.firstSample = j.value("first_sample", (uint64_t)0); s.audioFrames = j.value("audio_frames", (uint64_t)0);
    s.videoFrames = j.value("video_frames", (uint64_t)0); s.videoDups = j.value("video_dups", (uint64_t)0); s.bytes = j.value("bytes", (uint64_t)0);
    bool closed = ev == "segment_close"; s.complete = closed;
    bool found = false;
    for (auto& x : out) if (x.seg.index == s.index) { x.seg = s; x.active = !closed; found = true; }
    if (!found) { Entry en; en.seg = s; en.active = !closed; out.push_back(en); }
  }
  return out;
}
} // namespace ss
