#include "App/MediaWorker.h"

namespace ss::app {

ss::Status MediaWorker::start() {
    started_.store(true);
    return ss::okStatus();
}

void MediaWorker::stop() {
    unpublishCamera();
    if (adapters_.camera) adapters_.camera->close();
    started_.store(false);
}

bool MediaWorker::unpublishCamera() { return true; }

void MediaWorker::onProcessedBlock(const AudioBlockView&, const AudioBlockView&) {
    tapBlocks_.fetch_add(1, std::memory_order_relaxed);
}

ss::ActionResult MediaWorker::handleAction(const ss::Action&) {
    ss::ActionResult r;
    r.accepted = false;
    r.reason = "Recording and broadcast are not available in this build";
    return r;
}

void MediaWorker::fillView(MediaView& m, RecordView& r, BroadcastView& b) const {
    m.workerAvailable = false;
    m.note = "Media decoding is not implemented yet";
    if (adapters_.camera) m.cameraState = static_cast<unsigned char>(adapters_.camera->cameraState());
    r.state = RecordUiState::Off;
    r.note = "Recording is not implemented yet";
    b.state = BroadcastUiState::Off;
    b.note = "Broadcast is not implemented yet";
}

}  // namespace ss::app
