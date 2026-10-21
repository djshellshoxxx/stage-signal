// Sliding-window maximum/minimum over the last `window` pushed values (monotonic deque, O(1) amortised).
// Allocates only in init(); push() is real-time safe.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ss::dynamics {

template <bool kMax>
class SlidingExtreme {
public:
    void init(std::size_t window) {
        window_ = window < 1 ? 1 : window;
        cap_ = window_ + 1;
        pos_.assign(cap_, 0);
        val_.assign(cap_, 0.0f);
        reset();
    }
    void reset() { head_ = 0; count_ = 0; n_ = 0; }
    std::size_t window() const { return window_; }

    // Push v and return the extreme over the last `window` values including v.
    float push(float v) {
        while (count_ > 0) {   // drop dominated entries from the back
            const std::size_t b = (head_ + count_ - 1) % cap_;
            const bool dominated = kMax ? (val_[b] <= v) : (val_[b] >= v);
            if (!dominated) break;
            --count_;
        }
        const std::size_t t = (head_ + count_) % cap_;
        pos_[t] = n_;
        val_[t] = v;
        ++count_;
        while (pos_[head_] + window_ <= n_) { head_ = (head_ + 1) % cap_; --count_; }   // expired
        ++n_;
        return val_[head_];
    }

private:
    std::vector<std::uint64_t> pos_;
    std::vector<float> val_;
    std::size_t window_ = 1, cap_ = 2, head_ = 0, count_ = 0;
    std::uint64_t n_ = 0;
};

}  // namespace ss::dynamics
