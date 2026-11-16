// UI-facing recording status. Struct owner: engine-media-record. Producer: MediaWorker::fillView().
#pragma once
#include <cstdint>
#include <string>

namespace ss::app {

enum class RecordUiState : unsigned char { Off, Preparing, Recording, Stopping, Recoverable, Failed };

struct RecordView {
    RecordUiState state = RecordUiState::Off;
    std::uint64_t bytesWritten = 0, elapsedMs = 0;
    std::string note;
};

}  // namespace ss::app
