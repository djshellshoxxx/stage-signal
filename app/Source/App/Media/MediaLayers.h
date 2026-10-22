// Still-image and text layers for the compositor (IRenderLayerSource). Owner: engine-media-record.
// Control side publishes an immutable LayerSet; the RENDER thread picks the newest one without locking or allocating.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include "App/RenderHost.h"
#include "Engine/Media/ImageDecode.h"

namespace ss::app {

class MediaLayers final : public IRenderLayerSource {
public:
    static constexpr std::size_t kMax = ss::compose::Limits::kMaxMediaLayers;

    // Control side (any non-RT thread). Slot 0..kMax-2 hold images; the last slot is the text banner.
    ss::Status setImage(std::size_t slot, const std::string& path, ss::media::Tier tier, std::string* note = nullptr);
    ss::Status setDecodedFrame(std::size_t slot, std::shared_ptr<const PixelBuffer> frame);
    ss::Status setText(const std::string& utf8, std::uint32_t outW = 1280, std::uint32_t outH = 720);
    void clearSlot(std::size_t slot);
    void clearAll();
    std::uint32_t loaded() const;
    std::string lastNote() const;

    // RENDER thread.
    void fillLayers(OutputId, ss::Ns, ss::compose::FrameInputs& in) override;

private:
    struct Layer { std::shared_ptr<const ss::PixelBuffer> px; ss::compose::RectF region{0.f, 0.f, 1.f, 1.f}; };
    struct Set { std::array<Layer, kMax> layers; };
    void publish(const std::shared_ptr<Set>& s) { std::atomic_store(&cur_, std::shared_ptr<const Set>(s)); }
    std::shared_ptr<Set> copyCur() const;

    std::shared_ptr<const Set> cur_;
    std::shared_ptr<const Set> held_;          // RENDER thread only: keeps pixels alive until the next call
    mutable std::mutex ctlMutex_;
    std::string note_;
};

}  // namespace ss::app
