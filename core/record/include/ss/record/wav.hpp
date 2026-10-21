// StageSignal record: audio-only 24-bit PCM WAV path (REQ-R05). Needs no video renderer and no video encoder
// admission and no ffmpeg: the segmenting writer lives here, file access goes through small interfaces.
// Format: RIFF/WAVE with WAVE_FORMAT_EXTENSIBLE 24-bit PCM (68-byte header), classic 32-bit sizes, segmented
// before the 4 GiB RIFF limit. RF64 is not written (needs reader compatibility tests first, spec section 12).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/types.hpp"
#include "ss/record/journal.hpp"
#include "ss/record/naming.hpp"
#include "ss/record/profile.hpp"
#include "ss/record/segment.hpp"

namespace ss::record {

constexpr std::size_t kWavHeaderBytes = 68;

// dataBytes is clamped to what 32-bit RIFF sizes can express.
std::string makeWavHeader(std::uint32_t rateHz, std::uint32_t channels, std::uint64_t dataBytes);

struct WavHeaderInfo {
    std::uint32_t rateHz = 0, channels = 0, bitsPerSample = 0, blockAlign = 0;
    std::uint64_t dataOffset = 0;
    std::uint32_t declaredDataBytes = 0;
};
Result<WavHeaderInfo> parseWavHeader(const std::string& prefix);

// Header recovery: size the data chunk from the real file length (whole frames only).
struct WavRepair {
    std::uint64_t dataBytes = 0;       // repaired data length, multiple of blockAlign
    std::uint64_t truncateTo = 0;      // file length after dropping a trailing partial frame
    bool headerWasWrong = false;       // declared sizes did not match the file
    bool droppedPartialFrame = false;
};
Result<WavRepair> planWavRepair(const WavHeaderInfo& info, std::uint64_t fileSize);

// float32 [-1,1] -> 24-bit little-endian PCM, round to nearest, hard clamp (the limiter upstream owns loudness),
// NaN -> 0. out must hold n*3 bytes.
void pcm24FromFloat(const float* in, std::size_t n, std::uint8_t* out);

// ---- file access interfaces -------------------------------------------------------------------------------------
class ISegmentFile {
public:
    virtual ~ISegmentFile() = default;
    virtual Status write(const void* data, std::size_t n) = 0;
    virtual Status patch(std::uint64_t offset, const void* data, std::size_t n) = 0;    // rewrite bytes already written
    virtual Status flushDurable() = 0;                                                   // data + metadata to stable storage
    virtual Status close() = 0;
};
class IRecorderFs {
public:
    virtual ~IRecorderFs() = default;
    virtual Result<std::unique_ptr<ISegmentFile>> createFile(const std::string& name) = 0;   // name only, never a path
};

// stdio-backed implementation rooted at a directory (the directory must already exist and be writable).
class StdioRecorderFs final : public IRecorderFs, public IRecoveryFs {
public:
    explicit StdioRecorderFs(std::string dir) : dir_(std::move(dir)) {}
    Result<std::unique_ptr<ISegmentFile>> createFile(const std::string& name) override;
    bool fileSize(const std::string& name, std::uint64_t& size) override;
    Result<std::string> readPrefix(const std::string& name, std::size_t n) override;
    Status patch(const std::string& name, std::uint64_t offset, const std::string& bytes) override;
    Status truncate(const std::string& name, std::uint64_t newSize) override;
private:
    std::string path(const std::string& name) const;
    std::string dir_;
};

// ---- the audio-only recorder -------------------------------------------------------------------------------------
class AudioOnlyRecorder {
public:
    AudioOnlyRecorder(const RecordProfile& profile, std::string sessionId, Ns sessionStartWallUnixNs, IRecorderFs& fs,
                      IJournalSink& journal);
    Status start();                                   // opens segment 1 and writes the journal Begin/Open records
    Status write(const AudioBlockView& block);        // converts, splits at exact boundaries, rotates, flushes once a second
    Status stop(bool clean = true);                   // finalizes the open segment and journals End
    bool running() const { return running_; }

    std::uint64_t totalBytes() const { return totalBytes_; }
    std::uint64_t framesWritten() const { return framesWritten_; }
    Ns mediaNs() const { return samplesToNs(framesWritten_, rate_); }
    const std::vector<SegmentBound>& closedSegments() const { return seg_.closed(); }
    const std::vector<std::string>& fileNames() const { return files_; }
    std::uint64_t silenceFramesInserted() const { return silenceFrames_; }
    std::uint32_t gapCount() const { return gaps_; }

private:
    Status openSegment(std::uint32_t idx);
    Status closeSegment(Ns endNs);
    Status writeFrames(const std::uint8_t* pcm, std::uint64_t frames);
    Status flushNow();

    RecordProfile profile_;
    std::string sessionId_;
    Ns wallStart_;
    IRecorderFs& fs_;
    JournalWriter journal_;
    AudioSegmenter seg_;
    std::uint32_t rate_, channels_, bytesPerFrame_;
    std::unique_ptr<ISegmentFile> file_;
    std::vector<std::string> files_;
    std::vector<std::uint8_t> scratch_;
    std::vector<AudioSegmenter::Piece> pieces_;
    std::uint64_t segBytes_ = 0, totalBytes_ = 0, framesWritten_ = 0, silenceFrames_ = 0, sinceFlushFrames_ = 0;
    std::uint64_t expectedIndex_ = 0;
    Generation generation_ = 0;
    bool haveExpected_ = false, running_ = false;
    std::uint32_t gaps_ = 0, openIndex_ = 0;
};

}  // namespace ss::record
