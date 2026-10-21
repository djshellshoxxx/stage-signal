// UI-facing recording status. Struct owner: engine-media-record. Producer: MediaWorker::fillView().
#pragma once
#include <cstdint>
#include <string>

namespace ss::app {

enum class RecordUiState : unsigned char { Off, Preparing, Recording, Stopping, Recoverable, Failed };

struct RecordView {
    RecordUiState state = RecordUiState::Off;
    std::uint64_t bytesWritten = 0, elapsedMs = 0;
    bool videoOn = false;                // video file is being written next to the WAV
    std::uint32_t markers = 0, segments = 0;
    std::uint64_t freeDiskMb = 0;
    std::string note;                    // status or the actionable cause of a failure; no paths
};

}  // namespace ss::app
