#include "App/Media/MediaLayers.h"
#include <mutex>

namespace ss::app {

std::shared_ptr<MediaLayers::Set> MediaLayers::copyCur() const {
    auto c = std::atomic_load(&cur_);
    return c ? std::make_shared<Set>(*c) : std::make_shared<Set>();
}

ss::Status MediaLayers::setImage(std::size_t slot, const std::string& path, ss::media::Tier tier, std::string* note) {
    if (slot + 1 >= kMax) return ss::makeError(ss::ErrorCategory::InvalidArgument, "No such media slot");
    auto img = decodeImageFile(path, tier);
    if (!img.ok()) return img.error();
    std::lock_guard<std::mutex> l(ctlMutex_);
    auto s = copyCur();
    s->layers[slot].px = std::make_shared<const ss::PixelBuffer>(std::move(img.value().pixels));
    s->layers[slot].region = {0.f, 0.f, 1.f, 1.f};
    note_ = img.value().note;
    if (note) *note = note_;
    publish(s);
    return ss::okStatus();
}

ss::Status MediaLayers::setText(const std::string& utf8, std::uint32_t outW, std::uint32_t outH) {
    if (utf8.empty()) { clearSlot(kMax - 1); return ss::okStatus(); }
    auto px = rasterText(utf8, outW, std::max<std::uint32_t>(32, outH / 5));
    if (!px.ok()) return px.error();
    std::lock_guard<std::mutex> l(ctlMutex_);
    auto s = copyCur();
    s->layers[kMax - 1].px = std::make_shared<const ss::PixelBuffer>(std::move(px.value()));
    s->layers[kMax - 1].region = {0.f, 0.78f, 1.f, 0.2f};
    publish(s);
    return ss::okStatus();
}

void MediaLayers::clearSlot(std::size_t slot) {
    if (slot >= kMax) return;
    std::lock_guard<std::mutex> l(ctlMutex_);
    auto s = copyCur();
    s->layers[slot] = Layer{};
    publish(s);
}

void MediaLayers::clearAll() {
    std::lock_guard<std::mutex> l(ctlMutex_);
    publish(std::make_shared<Set>());
}

std::uint32_t MediaLayers::loaded() const {
    auto c = std::atomic_load(&cur_);
    std::uint32_t n = 0;
    if (c) for (auto& l : c->layers) n += l.px ? 1u : 0u;
    return n;
}

std::string MediaLayers::lastNote() const { std::lock_guard<std::mutex> l(ctlMutex_); return note_; }

void MediaLayers::fillLayers(OutputId, ss::Ns, ss::compose::FrameInputs& in) {
    held_ = std::atomic_load(&cur_);
    if (!held_) return;
    int n = in.mediaCount;
    for (const auto& l : held_->layers) {
        if (!l.px || n >= static_cast<int>(in.media.size())) continue;
        auto& m = in.media[static_cast<std::size_t>(n++)];
        m.frame = l.px.get();
        m.region = l.region;
        m.fit = ss::compose::FitMode::Fit;
        m.opacity = 1.f;
    }
    in.mediaCount = n;
}

}  // namespace ss::app
