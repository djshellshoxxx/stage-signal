// UI-facing broadcast status. Struct owner: engine-media-record. Producer: MediaWorker::fillView().
#pragma once
#include <cstdint>
#include <string>

namespace ss::app {

enum class BroadcastUiState : unsigned char { Off, Preparing, Live, Reconnecting, Stopping, Failed };

struct BroadcastView {
    BroadcastUiState state = BroadcastUiState::Off;
    std::uint32_t bitrateKbps = 0;
    std::uint64_t droppedFrames = 0;
    std::string note;           // redacted; never contains the stream key
};

}  // namespace ss::app
