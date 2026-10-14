// Mock/virtual IMidiPort for tests and other modules: records output, injects input, optional loopback,
// drop simulation and loss/restore simulation. Header-only, thread-safe, portable.
#pragma once
#include "core/ports.h"
#include <mutex>
#include <vector>

namespace ss {

class MockMidiPort : public IMidiPort {
 public:
  struct Sent { Ns t = 0; std::vector<uint8_t> bytes; };
  explicit MockMidiPort(const IClock* clock = nullptr) : clock_(clock) {}

  // ---- IMidiPort
  std::vector<MidiPortInfo> enumerate() override { std::lock_guard<std::mutex> l(m_); return ports_; }
  Status openInput(const std::string& id, std::function<void(Ns, const uint8_t*, size_t)> cb) override {
    std::lock_guard<std::mutex> l(m_);
    if (!inAlive_) return Status::Fail(Err::DeviceLost, "mock input lost");
    if (!known(id, true)) return Status::Fail(Err::InvalidArgument, "unknown input port");
    inCb_ = std::move(cb); inOpen_ = true; inId_ = id; return Status::Ok();
  }
  Status openOutput(const std::string& id) override {
    std::lock_guard<std::mutex> l(m_);
    if (!outAlive_) return Status::Fail(Err::DeviceLost, "mock output lost");
    if (!known(id, false)) return Status::Fail(Err::InvalidArgument, "unknown output port");
    outOpen_ = true; outId_ = id; return Status::Ok();
  }
  Status send(const uint8_t* d, size_t n) override {
    std::function<void(Ns, const uint8_t*, size_t)> cb; Ns t;
    {
      std::lock_guard<std::mutex> l(m_);
      if (!outOpen_ || !outAlive_) return Status::Fail(Err::DeviceLost, "mock output not open");
      ++attempts_;
      if (dropNext_ > 0) { --dropNext_; ++dropped_; return Status::Ok(); }                 // silent wire loss
      if (dropEvery_ > 0 && attempts_ % (uint64_t)dropEvery_ == 0) { ++dropped_; return Status::Ok(); }
      t = clock_ ? clock_->nowNs() : 0;
      sent_.push_back(Sent{t, std::vector<uint8_t>(d, d + n)});
      if (loopback_ && inOpen_ && inAlive_) cb = inCb_;
    }
    if (cb) cb(t, d, n);
    return Status::Ok();
  }
  void closeInput() override { std::lock_guard<std::mutex> l(m_); inOpen_ = false; inCb_ = nullptr; }
  void closeOutput() override { std::lock_guard<std::mutex> l(m_); outOpen_ = false; }
  bool inputAlive() const override { std::lock_guard<std::mutex> l(m_); return inOpen_ && inAlive_; }
  bool outputAlive() const override { std::lock_guard<std::mutex> l(m_); return outOpen_ && outAlive_; }
  void setLostCallback(std::function<void(bool)> cb) override { std::lock_guard<std::mutex> l(m_); lost_ = std::move(cb); }

  // ---- test controls
  void addPort(const MidiPortInfo& p) { std::lock_guard<std::mutex> l(m_); ports_.push_back(p); }
  void setLoopback(bool on) { std::lock_guard<std::mutex> l(m_); loopback_ = on; }     // output -> input
  void dropNext(int n) { std::lock_guard<std::mutex> l(m_); dropNext_ = n; }
  void setDropEvery(int n) { std::lock_guard<std::mutex> l(m_); dropEvery_ = n; }      // drop every n-th send attempt
  // Delivers bytes to the open input callback (arrival < 0 => clock now or 0). False if the input is not alive.
  bool injectInput(const uint8_t* d, size_t n, Ns arrival = -1) {
    std::function<void(Ns, const uint8_t*, size_t)> cb; Ns t;
    { std::lock_guard<std::mutex> l(m_);
      if (!inOpen_ || !inAlive_) return false;
      cb = inCb_; t = arrival >= 0 ? arrival : (clock_ ? clock_->nowNs() : 0); }
    if (cb) cb(t, d, n);
    return true;
  }
  bool injectInput(const std::vector<uint8_t>& v, Ns arrival = -1) { return injectInput(v.data(), v.size(), arrival); }
  // Simulates device removal: the port closes, then the lost callback fires (input flag).
  void simulateLoss(bool input) {
    std::function<void(bool)> cb;
    { std::lock_guard<std::mutex> l(m_);
      if (input) { inAlive_ = false; inOpen_ = false; inCb_ = nullptr; } else { outAlive_ = false; outOpen_ = false; }
      cb = lost_; }
    if (cb) cb(input);
  }
  void restore() { std::lock_guard<std::mutex> l(m_); inAlive_ = outAlive_ = true; }  // device back; caller must re-open
  std::vector<Sent> sent() const { std::lock_guard<std::mutex> l(m_); return sent_; }
  size_t sentCount() const { std::lock_guard<std::mutex> l(m_); return sent_.size(); }
  void clearSent() { std::lock_guard<std::mutex> l(m_); sent_.clear(); }
  uint64_t dropped() const { std::lock_guard<std::mutex> l(m_); return dropped_; }
  uint64_t attempts() const { std::lock_guard<std::mutex> l(m_); return attempts_; }

 private:
  bool known(const std::string& id, bool input) const {
    if (ports_.empty()) return true;
    for (auto& p : ports_) if (p.stableId == id && p.input == input) return true;
    return false;
  }
  const IClock* clock_;
  mutable std::mutex m_;
  std::vector<MidiPortInfo> ports_;
  std::vector<Sent> sent_;
  std::function<void(Ns, const uint8_t*, size_t)> inCb_;
  std::function<void(bool)> lost_;
  std::string inId_, outId_;
  bool inOpen_ = false, outOpen_ = false, inAlive_ = true, outAlive_ = true, loopback_ = false;
  int dropNext_ = 0, dropEvery_ = 0; uint64_t dropped_ = 0, attempts_ = 0;
};

} // namespace ss
