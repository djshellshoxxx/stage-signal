// StageSignal record: segmentation (spec section 12: "15 min or size budget, whichever first, with exact boundary
// timestamps and no overlap/loss").
//  * Audio-only: AudioSegmenter cuts on exact sample indices; boundary time = samplesToNs(frame, rate).
//  * Video via ffmpeg's segment muxer: boundaries are forced at keyframes (see ffmpeg_args.hpp); the ACTUAL boundaries
//    are read back from the muxer's segment list (parseSegmentListCsv) and checked for gaps/overlaps.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/time.hpp"
#include "ss/record/profile.hpp"

namespace ss::record {

enum class CutReason : std::uint8_t { Time, Size, Stop };

struct SegmentBound {
    std::uint32_t index = 0;                 // 1-based
    std::uint64_t startFrame = 0, frames = 0;
    Ns startNs = 0, endNs = 0;               // session media time, exact: samplesToNs(startFrame / startFrame + frames)
    CutReason reason = CutReason::Time;
};

class AudioSegmenter {
public:
    // headerBytes counts toward the size budget (WAV header). bytesPerFrame = channels * 3 for 24-bit PCM.
    AudioSegmenter(std::uint32_t rateHz, Ns segmentNs, std::uint64_t sizeBudgetBytes, std::uint32_t bytesPerFrame,
                   std::uint64_t headerBytes);

    struct Piece {
        std::uint32_t segmentIndex = 0;
        std::uint64_t frames = 0;            // frames of the input block that belong to this segment (>0)
        bool closesSegment = false;          // this piece fills the segment; the next piece opens a new one
        CutReason reason = CutReason::Time;
    };
    // Splits a block of `frames` into per-segment pieces. Pieces sum to exactly `frames`: nothing lost, nothing repeated.
    void split(std::uint64_t frames, std::vector<Piece>& out);
    // Closes the segment in progress (Stop) and returns its bounds. Returns frames==0 bounds if nothing was written.
    SegmentBound finish();

    std::uint32_t currentIndex() const { return index_; }
    std::uint64_t framesInCurrent() const { return inCurrent_; }
    std::uint64_t framesPerSegment() const { return perSegment_; }
    CutReason plannedReason() const { return reason_; }
    const std::vector<SegmentBound>& closed() const { return closed_; }

private:
    void closeCurrent(CutReason r);
    std::uint32_t rate_;
    std::uint64_t perSegment_ = 0;
    CutReason reason_ = CutReason::Time;
    std::uint32_t index_ = 1;
    std::uint64_t startFrame_ = 0, inCurrent_ = 0;
    std::vector<SegmentBound> closed_;
};

// Segment duration for the ffmpeg segment muxer: min(profile segment length, size budget / pessimistic byte rate),
// floored at 10 s. The segment muxer cannot cut by size, so video size budgets are converted to time.
Ns videoSegmentDuration(Ns segmentNs, std::uint64_t sizeBudgetBytes, double upperBytesPerSec);

struct SegmentListEntry { std::string file; Ns startNs = 0, endNs = 0; };
// ffmpeg `-segment_list_type csv`: "file,start_seconds,end_seconds" per line. Malformed lines are skipped and counted.
std::vector<SegmentListEntry> parseSegmentListCsv(const std::string& text, std::size_t* malformedLines = nullptr);
// Reports every gap or overlap larger than toleranceNs between consecutive entries (empty = contiguous).
std::vector<std::string> checkContinuity(const std::vector<SegmentListEntry>& entries, Ns toleranceNs);

}  // namespace ss::record
