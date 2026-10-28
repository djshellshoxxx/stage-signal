#include "App/Media/CameraFeed.h"
#include <algorithm>

namespace ss::app {
CameraFeed::CameraFeed(std::shared_ptr<ICameraSource> source) : source_(std::move(source)) {
    layer_.prepare(1280, 720);
}
ss::Status CameraFeed::open(const std::string& id) {
    close();
    std::lock_guard<std::mutex> lock(controlMutex_);
    std::atomic_store(&id_, std::make_shared<const std::string>(id)); lostAt_.store(0); generation_.fetch_add(1); enabled_.store(true);
    auto status = source_->open(id, this);
    if (!status.ok()) enabled_.store(false);
    return status;
}
void CameraFeed::close() {
    published_.store(false);
    std::lock_guard<std::mutex> lock(controlMutex_);
    enabled_.store(false);
    source_->close();
    std::atomic_store(&newest_, std::shared_ptr<const Frame>());
    lostAt_.store(0); std::atomic_store(&id_, std::shared_ptr<const std::string>()); generation_.fetch_add(1);
}
void CameraFeed::poll(Ns now) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    source_->poll();
    auto frame = std::atomic_load(&newest_);
    if (enabled_.load() && frame && now > frame->at + msToNs(250)) {
        Ns expected = 0;
        lostAt_.compare_exchange_strong(expected, frame->at + msToNs(250));
    }
}
ss::Status CameraFeed::publish(OutputId target) {
    if (!enabled_.load() || !std::atomic_load(&newest_) || lostAt_.load() != 0)
        return ss::makeError(ss::ErrorCategory::DeviceLost, "Preview the selected camera before publishing it");
    target_.store(target); published_.store(true);
    return ss::okStatus();
}
CameraState CameraFeed::state() const {
    if (!enabled_.load()) return source_->cameraState() == CameraState::Unsupported ? CameraState::Unsupported : CameraState::Off;
    if (lostAt_.load() != 0) return CameraState::Lost;
    return published_.load() ? CameraState::Published : CameraState::Previewing;
}
std::shared_ptr<const PixelBuffer> CameraFeed::preview() const {
    auto frame = std::atomic_load(&newest_);
    if (!frame || !enabled_.load()) return {};
    return std::shared_ptr<const PixelBuffer>(frame, &frame->pixels);
}
void CameraFeed::onCameraFrame(const std::string& id, const PixelBuffer& pixels, Ns now) {
    // Native source invokes this on MEDIA after conversion. At most the newest immutable frame is retained.
    const auto selected = std::atomic_load(&id_);
    if (!enabled_.load() || !selected || id != *selected || pixels.empty() || pixels.width > 1280 || pixels.height > 720
        || pixels.rgba.size() != std::size_t(pixels.width) * pixels.height * 4) return;
    auto frame = std::make_shared<Frame>();
    frame->pixels = pixels; frame->at = now; frame->generation = generation_.load();
    std::atomic_store(&newest_, std::shared_ptr<const Frame>(std::move(frame)));
    lostAt_.store(0);
}
void CameraFeed::onCameraLost(const std::string& id, Ns now) {
    const auto selected = std::atomic_load(&id_);
    if (!enabled_.load() || !selected || id != *selected) return;
    Ns expected = 0;
    lostAt_.compare_exchange_strong(expected, std::max<Ns>(1, now));
}
void CameraFeed::fillLayers(OutputId output, Ns now, ss::compose::FrameInputs& in) {
    if (!enabled_.load()) { in.camera = nullptr; return; }
    const auto generation = generation_.load();
    if (generation != renderedGeneration_) {
        layer_.prepare(1280, 720);  // reset stale source without reallocating reserved buffers
        ss::compose::CameraLayerConfig cfg;
        cfg.sourceId = static_cast<std::uint32_t>(generation);
        layer_.configure(cfg);
        rendered_.reset(); renderedGeneration_ = generation;
    }
    auto frame = std::atomic_load(&newest_);
    if (frame && frame->generation == generation && frame != rendered_) {
        layer_.pushFrame(static_cast<std::uint32_t>(generation), frame->pixels, frame->at);
        rendered_ = std::move(frame);
    }
    const Ns lost = lostAt_.load();
    if (lost != 0) layer_.markLost(lost);
    const bool routed = target_.load() == kAllOutputs || target_.load() == output;
    layer_.setPublished(published_.load() && routed);
    layer_.update(now);
    in.camera = &layer_;
}
}  // namespace ss::app
