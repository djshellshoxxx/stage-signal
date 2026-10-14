// Deterministic IVideoDecoder for tests and for renderer/UI development (no files, no codecs).
// Frame i pixel(x,y) = rgb(R = i & 255, G = x*255/(w-1), B = y*255/(h-1)); pts = i / fps.
#pragma once
#include "core/ports.h"
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>

namespace ss { namespace media {

struct FakeVideoParams {
  int w = 64, h = 48, frames = 30; double fps = 10.0;
  bool failOpen = false;
  int throwAtFrame = -1;                          // nextFrame throws std::runtime_error at this frame index
  int failAtFrame = -1;                           // nextFrame returns false (early EOF/fail) at this frame index
  std::function<void(int)> beforeFrame;           // test hook: may block (simulates a hung decoder)
};
struct FakeVideoCounters {
  std::atomic<int> opened{0}, closed{0}, seeks{0}, framesDecoded{0};
};

class FakeVideoDecoder : public IVideoDecoder {
 public:
  explicit FakeVideoDecoder(FakeVideoParams p, std::shared_ptr<FakeVideoCounters> c = nullptr) : p_(std::move(p)), c_(c ? c : std::make_shared<FakeVideoCounters>()) {}
  Status open(const std::string&, int maxW, int maxH) override {
    if (p_.failOpen) return Status::Fail(Err::DecodeFailure, "fake open failure");
    w_ = p_.w < maxW ? p_.w : maxW; h_ = p_.h < maxH ? p_.h : maxH; if (w_ < 1) w_ = 1; if (h_ < 1) h_ = 1;
    idx_ = 0; open_ = true; ++c_->opened; return Status::Ok();
  }
  bool nextFrame(Frame& out, Ns& ptsNs) override {
    if (!open_ || idx_ >= p_.frames) return false;
    if (p_.beforeFrame) p_.beforeFrame(idx_);
    if (idx_ == p_.throwAtFrame) throw std::runtime_error("fake decoder fault");
    if (idx_ == p_.failAtFrame) return false;
    out.resize(w_, h_);
    for (int y = 0; y < h_; ++y)
      for (int x = 0; x < w_; ++x)
        out.px[(size_t)y * w_ + x] = rgb(idx_ & 255, w_ > 1 ? x * 255 / (w_ - 1) : 0, h_ > 1 ? y * 255 / (h_ - 1) : 0);
    ptsNs = (Ns)((double)idx_ / p_.fps * 1e9 + 0.5); out.pts = ptsNs;
    ++idx_; ++c_->framesDecoded; return true;
  }
  Status seekStart() override { idx_ = 0; ++c_->seeks; return Status::Ok(); }
  double durationSec() const override { return (double)p_.frames / p_.fps; }
  void close() override { if (open_) { open_ = false; ++c_->closed; } }
  std::shared_ptr<FakeVideoCounters> counters() const { return c_; }
 private:
  FakeVideoParams p_; std::shared_ptr<FakeVideoCounters> c_;
  int w_ = 0, h_ = 0, idx_ = 0; bool open_ = false;
};

}} // namespace ss::media
