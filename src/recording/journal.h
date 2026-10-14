// MOD-11 segment journal (append-only JSON lines, flushed per event). Lists completed and active segments so a crash
// leaves an authoritative record of what to recover. Only complete lines are trusted when parsing.
#pragma once
#include "recording/io.h"
#include <memory>
namespace ss {

struct SegmentInfo {
  int index = 0;
  std::string path;
  std::string kind;                // "wav" | "avi" | "mkv"
  Ns startNs = 0;                  // session-timeline start of first audio sample
  Ns endNs = 0;                    // session-timeline end (exclusive) of last audio sample
  uint64_t firstSample = 0;        // first audio sample index (session timeline, contiguous across segments)
  uint64_t audioFrames = 0;
  uint64_t videoFrames = 0;        // frame slots written (incl. duplicates)
  uint64_t videoDups = 0;          // duplicated slots (zero-length AVI chunk / none in MKV)
  uint64_t bytes = 0;
  bool complete = false;           // finalized cleanly (or repaired by recovery)
  bool recovered = false;
};

class SegmentJournal {
 public:
  // Opens (append) the journal file. If it cannot be opened every call returns that failure (never silently ignored).
  Status open(IFileSystem& fs, const std::string& path);
  Status session(const std::string& sessionId, const std::string& show, int64_t startUnixMs, const std::string& container);
  Status segmentOpen(const SegmentInfo& s);
  Status segmentClose(const SegmentInfo& s);
  Status note(const std::string& event, const std::string& detail);   // e.g. gap, lost_interval, stop reason
  Status close();
  const std::string& path() const { return path_; }

  struct Entry { SegmentInfo seg; bool active = true; };
  // Parse journal bytes: last state per segment index; torn last line ignored.
  static std::vector<Entry> parse(const rec::Bytes& bytes);
 private:
  Status line(const std::string& s);
  std::unique_ptr<IFileSink> sink_; std::string path_;
};

} // namespace ss
