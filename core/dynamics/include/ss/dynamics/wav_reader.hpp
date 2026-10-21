// Minimal WAV header parser, test-oriented PCM reader and truncated-file recovery (REQ-R05).
// The reader exists to verify round trips and to implement recovery; it is not a general-purpose WAV decoder
// (PCM only, 16/24 bit, classic RIFF, no RF64/extensible).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"

namespace ss::dynamics {

struct WavInfo {
    std::uint16_t channels = 0, bitsPerSample = 0;
    std::uint32_t sampleRateHz = 0;
    std::uint64_t dataOffset = 0;        // byte offset of the first audio byte
    std::uint64_t claimedDataBytes = 0;  // value stored in the data chunk header (may be a placeholder)
    std::uint64_t claimedRiffBytes = 0;
    std::uint64_t availableDataBytes = 0;   // bytes actually present after dataOffset
    std::uint16_t blockAlign() const { return static_cast<std::uint16_t>(channels * (bitsPerSample / 8)); }
    // true when the sizes in the header agree with the file length (a cleanly finalised file)
    bool consistent(std::uint64_t fileBytes) const;
};

// Parses RIFF/fmt/data up to the start of audio. Fails with DecodeFailure when no PCM audio chunk is present.
Result<WavInfo> parseWavHeader(const std::uint8_t* data, std::size_t n, std::uint64_t totalFileBytes);

struct WavData {
    WavInfo info;
    std::vector<std::int32_t> samples;   // interleaved signed integers (24-bit values sign-extended)
    std::uint64_t frames() const { return info.channels ? samples.size() / info.channels : 0; }
};

// Strict decode: header sizes must match the file length, otherwise DecodeFailure (use recoverWav first).
Result<WavData> readWav(const std::vector<std::uint8_t>& file);
// Convenience: integer samples -> float exactly as the writer's inverse (q / 2^(bits-1)).
std::vector<float> wavToFloat(const WavData& w);

struct RecoveryHints {
    bool hasExpectedFrames = false;      // from the journal: frames known to have been flushed with a header patch
    std::uint64_t expectedFrames = 0;
};

struct WavRecoveryReport {
    bool headerWasConsistent = false;    // nothing needed repair
    std::uint64_t claimedFrames = 0;     // frames the old header claimed (placeholder -> 0)
    std::uint64_t framesRecovered = 0;
    std::uint64_t partialFrameBytesDropped = 0;
    bool lostTailKnown = false;          // true when a journal figure allows quantifying the loss
    std::uint64_t framesLostAtLeast = 0; // expectedFrames - recovered, when positive
    double secondsRecovered = 0;
    std::string disclosure;              // human-readable statement about the unflushed tail
};

// Repairs an in-memory file: data size is derived from the file length (assumes the data chunk is last), a trailing
// partial frame is cut, RIFF/data sizes are rewritten. Never invents audio.
Result<WavRecoveryReport> recoverWav(std::vector<std::uint8_t>& file, const RecoveryHints& hints = {});
// Same on disk (header rewritten in place, partial frame truncated). Error texts never contain the path.
Result<WavRecoveryReport> recoverWavFile(const std::string& path, const RecoveryHints& hints = {});

}  // namespace ss::dynamics
