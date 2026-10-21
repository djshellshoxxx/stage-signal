#include "ss/compose/frame_pool.hpp"
#include "ss/compose/raster.hpp"

namespace ss::compose {

ss::Status FramePool::prepare(std::uint32_t w, std::uint32_t h, std::size_t count) {
    if (!validDims(w, h) || count == 0 || count > Limits::kMaxFramePool)
        return ss::makeError(ss::ErrorCategory::InvalidArgument, "frame pool size out of range");
    for (std::size_t i = 0; i < Limits::kMaxFramePool; ++i) {
        busy_[i].store(false);
        if (i < count) frames_[i] = PixelBuffer(w, h);
        else frames_[i] = PixelBuffer();
    }
    count_ = count;
    return ss::okStatus();
}

int FramePool::acquire() {
    for (std::size_t i = 0; i < count_; ++i) {
        bool expected = false;
        if (busy_[i].compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return int(i);
    }
    return -1;
}

void FramePool::release(int slot) {
    if (slot >= 0 && std::size_t(slot) < count_) busy_[std::size_t(slot)].store(false, std::memory_order_release);
}

std::size_t FramePool::freeCount() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < count_; ++i) n += busy_[i].load(std::memory_order_acquire) ? 0 : 1;
    return n;
}

}  // namespace ss::compose
