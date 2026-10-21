// Fixed-size pool of output frames. Allocated once in prepare(); acquire/release are lock-free and never allocate.
// Producer (render thread) acquires + fills; consumer (present/encode thread) releases when done.
#pragma once
#include <array>
#include <atomic>
#include "ss/base/error.hpp"
#include "ss/compose/limits.hpp"
#include "ss/compose/pixel.hpp"

namespace ss::compose {

class FramePool {
public:
    FramePool() = default;
    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;

    ss::Status prepare(std::uint32_t w, std::uint32_t h, std::size_t count);
    int acquire();                       // slot index or -1 when exhausted (caller counts a drop)
    void release(int slot);
    PixelBuffer& at(int slot) { return frames_[std::size_t(slot)]; }
    const PixelBuffer& at(int slot) const { return frames_[std::size_t(slot)]; }
    std::size_t capacity() const { return count_; }
    std::size_t freeCount() const;

private:
    std::array<PixelBuffer, Limits::kMaxFramePool> frames_;
    std::array<std::atomic<bool>, Limits::kMaxFramePool> busy_{};
    std::size_t count_ = 0;
};

}  // namespace ss::compose
