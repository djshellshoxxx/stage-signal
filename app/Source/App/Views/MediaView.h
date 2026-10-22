// UI-facing media/camera/text status. Struct owner: engine-media-record. Producer: MediaWorker::fillView().
#pragma once
#include <cstdint>
#include <string>

namespace ss::app {

struct MediaView {
    bool workerAvailable = false;        // true once the MEDIA worker thread runs (stills decode; GIF first frame only; no video)
    std::uint32_t loadedItems = 0;
    unsigned char cameraState = 0;       // ss::app::CameraState value
    bool ffmpegFound = false;            // video recording/broadcast possible
    std::string ffmpegNote;              // cause when not found, or a short version line
    std::string note;
};

}  // namespace ss::app
