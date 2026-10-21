// UI-facing media/camera/text status. Struct owner: engine-media-record. Producer: MediaWorker::fillView().
#pragma once
#include <cstdint>
#include <string>

namespace ss::app {

struct MediaView {
    bool workerAvailable = false;        // false in the contract commit: decoders are not implemented
    std::uint32_t loadedItems = 0;
    unsigned char cameraState = 0;       // ss::app::CameraState value
    std::string note;
};

}  // namespace ss::app
