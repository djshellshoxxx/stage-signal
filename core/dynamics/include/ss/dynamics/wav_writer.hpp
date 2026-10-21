// 24-bit PCM RIFF/WAVE segmented writer (REQ-R05, spec section 12 "Recording formats and lifecycle", section 8 gaps).
//
// Quantisation rule (documented, bit-exact for 24-bit-representable input):
//   q = floor(x * 8388608 + 0.5) clamped to [-8388608, 8388607]; NaN -> 0; stored as 3 bytes little-endian two's complement.
//   Decode is q / 8388608. Any float that is an exact multiple of 2^-23 with |x| < 1 round-trips bit-exactly.
//   Dither is OFF by default; optional TPDF dither (+-1 LSB, deterministic ss::Rng seed) is applied before rounding.
//
// Segmenting: a segment holds at most min(segmentSeconds * rate, (maxSegmentBytes - 44) / frameBytes) frames. Boundaries are
// absolute stream sample indices: segment k covers [start + k*segFrames, start + (k+1)*segFrames). No overlap, no loss.
// The classic RIFF 32-bit size field limits a file to 4 GiB - 1; the default byte budget stays below it.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/rng.hpp"
#include "ss/base/time.hpp"

namespace ss::dynamics {

constexpr std::uint32_t kWavHeaderBytes = 44;
constexpr std::uint64_t kRiffMaxFileBytes = 0xFFFFFFFFULL;                   // classic RIFF limit
constexpr std::uint64_t kDefaultSegmentBytes = kRiffMaxFileBytes - (1u << 20);  // 4 GiB - 1 MiB margin

// ---- byte sinks ----------------------------------------------------------------------------------
class IByteSink {
public:
    virtual ~IByteSink() = default;
    virtual Status write(const std::uint8_t* data, std::size_t n) = 0;                  // append
    virtual Status patch(std::uint64_t offset, const std::uint8_t* data, std::size_t n) = 0;  // overwrite written bytes
    virtual Status flush() = 0;
    virtual Status close() = 0;
    virtual std::uint64_t size() const = 0;                                              // bytes written so far
};

class MemorySink final : public IByteSink {
public:
    explicit MemorySink(std::shared_ptr<std::vector<std::uint8_t>> store = nullptr)
        : store_(store ? std::move(store) : std::make_shared<std::vector<std::uint8_t>>()) {}
    Status write(const std::uint8_t* data, std::size_t n) override;
    Status patch(std::uint64_t offset, const std::uint8_t* data, std::size_t n) override;
    Status flush() override { return okStatus(); }
    Status close() override { return okStatus(); }
    std::uint64_t size() const override { return store_->size(); }
    const std::vector<std::uint8_t>& bytes() const { return *store_; }
    std::shared_ptr<std::vector<std::uint8_t>> store() const { return store_; }
private:
    std::shared_ptr<std::vector<std::uint8_t>> store_;
};

class FileSink final : public IByteSink {
public:
    FileSink();
    ~FileSink() override;
    Status open(const std::string& path);                  // truncates; error text never includes the path
    Status write(const std::uint8_t* data, std::size_t n) override;
    Status patch(std::uint64_t offset, const std::uint8_t* data, std::size_t n) override;
    Status flush() override;
    Status close() override;
    std::uint64_t size() const override { return size_; }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint64_t size_ = 0;
};

// ---- segment bookkeeping -------------------------------------------------------------------------
struct SegmentInfo {
    std::uint32_t index = 0;
    std::uint64_t firstSample = 0;      // absolute stream sample index of the first frame
    Ns startNs = 0;                     // samplesToNs(firstSample - streamStart): session-relative
};

struct OpenedSink {
    std::unique_ptr<IByteSink> sink;
    std::string name;                   // file name or label recorded in the journal
};

class ISinkFactory {
public:
    virtual ~ISinkFactory() = default;
    virtual Result<OpenedSink> open(const SegmentInfo& info) = 0;
};

// Keeps every segment in memory; for tests and tools.
class MemorySinkFactory final : public ISinkFactory {
public:
    Result<OpenedSink> open(const SegmentInfo& info) override;
    std::vector<std::shared_ptr<std::vector<std::uint8_t>>> segments;
    int failOpenAtIndex = -1;           // test hook: segment index whose open() fails (DiskFull)
};

// Files named <dir>/<base>_<segment:04>.wav (base from makeRecordingBaseName()).
class FileSinkFactory final : public ISinkFactory {
public:
    FileSinkFactory(std::string directory, std::string baseName)
        : dir_(std::move(directory)), base_(std::move(baseName)) {}
    Result<OpenedSink> open(const SegmentInfo& info) override;
private:
    std::string dir_, base_;
};

// "show-name sanitized" -> [A-Za-z0-9_-]+ (max 48 chars, "show" if nothing remains).
std::string sanitizeShowName(const std::string& name);
// "20260704T153000Z" from a unix wall clock in ns (UTC).
std::string formatUtcCompact(Ns wallUnixNs);
// "<UTC>_<session>_<show>" with sanitised parts; the UI shows local time separately.
std::string makeRecordingBaseName(Ns wallUnixNs, const std::string& sessionId, const std::string& showName);

// ---- writer ------------------------------------------------------------------------------------
struct WavWriterConfig {
    std::uint32_t sampleRateHz = 48000;
    std::uint16_t channels = 2;                         // 1 or 2
    double segmentSeconds = 900.0;                      // 15 min
    std::uint64_t maxSegmentBytes = kDefaultSegmentBytes;   // file size budget incl. 44-byte header
    double maxGapMs = 250.0;                            // gaps <= this are filled with timestamped silence
    std::uint32_t maxGapsInWindow = 3;                  // a 4th gap inside gapWindowSeconds stops recording
    double gapWindowSeconds = 60.0;
    bool dither = false;                                // TPDF, off by default
    std::uint64_t ditherSeed = 1;
    std::uint32_t bufferFrames = 8192;                  // internal staging; the unflushed tail is at most this + sink buffers
    std::uint64_t headerPatchIntervalFrames = 0;        // 0 = every 2 s of audio; header is also patched by flush()
};

enum class SegmentState : std::uint8_t { Active, Completed, Failed };

struct SegmentJournalEntry {
    std::uint32_t index = 0;
    std::string name;
    SegmentState state = SegmentState::Active;
    std::uint64_t firstSample = 0;      // inclusive absolute sample index
    std::uint64_t endSample = 0;        // exclusive; equals firstSample + frames
    std::uint64_t frames = 0;           // frames accepted into this segment (including inserted silence)
    std::uint64_t flushedFrames = 0;    // frames handed to the sink with a matching header patch
    std::uint64_t bytes = 0;            // data bytes accepted
    Ns startNs = 0, endNs = 0;
    std::uint32_t gapCount = 0;         // gaps whose silence started in this segment
    bool headerFinalised = false;
};

enum class GapKind : std::uint8_t { SilenceInserted, OverlapTrimmed };

struct GapEvent {
    GapKind kind = GapKind::SilenceInserted;
    std::uint64_t startSample = 0;      // absolute index of the first missing (or duplicated) frame
    std::uint64_t frames = 0;
    Ns startNs = 0, durationNs = 0;     // session-relative
    std::uint32_t segmentIndex = 0;
    std::string label;                  // e.g. "gap: 120 ms of silence inserted"
};

class WavSegmentWriter {
public:
    WavSegmentWriter(const WavWriterConfig& cfg, ISinkFactory& factory);
    ~WavSegmentWriter();

    // Validates the config (bounds before allocation) and opens segment 0 lazily at the first frame.
    // firstSampleIndex is the absolute capture counter value of the first frame that will be written.
    Status start(std::uint64_t firstSampleIndex = 0);
    // Interleaved float frames. firstSampleIndex is the capture counter of frames[0]; a jump forward is a gap.
    Status write(const float* interleaved, std::uint32_t frames, std::uint64_t firstSampleIndex, std::uint32_t audioFlags = 0);
    Status flush();                     // drain staging, patch the open header, flush the sink
    Status stop();                      // finalise the open segment (header sizes), idempotent

    bool failed() const { return failed_; }
    const Error& failure() const { return failure_; }
    std::uint64_t nextSampleIndex() const { return next_; }
    std::uint64_t segmentFrames() const { return segLimit_; }
    const std::vector<SegmentJournalEntry>& journal() const { return journal_; }
    const std::vector<GapEvent>& gaps() const { return gaps_; }
    std::uint64_t gapEventsDropped() const { return gapsDropped_; }
    std::uint64_t discontinuityFlags() const { return discontinuityFlags_; }
    std::uint64_t totalFramesWritten() const { return totalFrames_; }
    std::uint64_t clippedSamples() const { return clipped_; }

    // Called (on the writer's thread) whenever a journal entry opens, flushes or closes, and for each gap event.
    std::function<void(const SegmentJournalEntry&)> onJournal;
    std::function<void(const GapEvent&)> onGap;

private:
    Status fail(Error e);
    Status openSegment();
    Status closeSegment();
    Status drain();
    Status patchHeader(std::uint64_t frames);
    Status writeFrames(const float* x, std::uint64_t frames);   // x == nullptr writes silence
    void notify();

    WavWriterConfig cfg_;
    ISinkFactory& factory_;
    std::uint32_t frameBytes_ = 6;
    std::uint64_t segLimit_ = 0;
    bool started_ = false, failed_ = false, stopped_ = false;
    Error failure_;
    std::uint64_t streamStart_ = 0, next_ = 0, totalFrames_ = 0, clipped_ = 0, gapsDropped_ = 0, discontinuityFlags_ = 0;
    std::uint32_t segIndex_ = 0;
    std::unique_ptr<IByteSink> sink_;
    std::vector<std::uint8_t> buf_;
    std::size_t bufLen_ = 0;
    std::uint64_t segFrames_ = 0, patchedFrames_ = 0, patchInterval_ = 0;
    std::vector<SegmentJournalEntry> journal_;
    std::vector<GapEvent> gaps_;
    std::vector<std::uint64_t> recentGapStarts_;
    Rng rng_;
};

}  // namespace ss::dynamics
