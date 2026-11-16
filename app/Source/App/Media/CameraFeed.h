// Privacy gate between a selected capture device, operator preview and the compositor.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include "App/RenderHost.h"
#include "Engine/ICameraSource.h"

namespace ss::app {
class CameraFeed final : public ICameraSink, public IRenderLayerSource {
public:
    explicit CameraFeed(std::shared_ptr<ICameraSource> source);
    ~CameraFeed() override { close(); }
    std::vector<CameraDesc> devices() { std::lock_guard<std::mutex> lock(controlMutex_); return source_->listCameras(); }
    ss::Status open(const std::string& id);
    void close();
    void poll(Ns now);
    ss::Status publish(OutputId target);
    void unpublish() { published_.store(false); }
    CameraState state() const;
    std::shared_ptr<const PixelBuffer> preview() const;
    void onCameraFrame(const std::string& id, const PixelBuffer& frame, Ns now) override;
    void onCameraLost(const std::string& id, Ns now) override;
    void fillLayers(OutputId output, Ns now, ss::compose::FrameInputs& in) override;
private:
    struct Frame { PixelBuffer pixels; Ns at = 0; std::uint64_t generation = 0; };
    std::shared_ptr<ICameraSource> source_;
    mutable std::mutex controlMutex_;
    std::shared_ptr<const std::string> id_;
    std::atomic<bool> enabled_{false}, published_{false};
    std::atomic<OutputId> target_{kPrimaryOutput};
    std::atomic<Ns> lostAt_{0};
    std::atomic<std::uint64_t> generation_{0};
    std::shared_ptr<const Frame> newest_;
    ss::compose::CameraLayer layer_;  // RENDER thread only after construction
    std::shared_ptr<const Frame> rendered_;
    std::uint64_t renderedGeneration_ = ~std::uint64_t(0);
};
}  // namespace ss::app
