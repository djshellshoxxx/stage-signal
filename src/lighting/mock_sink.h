// MOD-10 mock datagram sinks (REQ-L03). None of these touch a socket.
//  - RecordingSink : records every datagram (bounded); failure injection; doubles as the "real sink spy" in tests.
//  - CountingSink  : counts datagrams/bytes only.
//  - DryRunSink    : used by dry-run/preview/benchmark engines. Accepts frames, counts what WOULD have been
//                    sent, and ALWAYS reports datagramsSent()==0 (nothing ever leaves the process).
#pragma once
#include "core/ports.h"
#include <mutex>
#include <string>
#include <vector>

namespace ss {

struct Datagram { std::string host; uint16_t port = 0; std::vector<uint8_t> bytes; };

class RecordingSink : public IDatagramSink {
 public:
  explicit RecordingSink(size_t maxRecords = 16384) : max_(maxRecords) {}
  Status open(const std::string& nic, bool allowBroadcast) override {
    std::lock_guard<std::mutex> l(m_);
    if (failOpen_) return Status::Fail(Err::TransportFailure, "injected open failure");
    open_ = true; nic_ = nic; broadcast_ = allowBroadcast; ++opens_; return Status::Ok();
  }
  Status sendTo(const std::string& host, uint16_t port, const uint8_t* d, size_t n) override {
    std::lock_guard<std::mutex> l(m_);
    if (!open_) return Status::Fail(Err::DeviceLost, "sink not open");
    if (failing_) return Status::Fail(Err::TransportFailure, "injected send failure");
    ++sent_;
    if (log_.size() < max_) { Datagram g; g.host = host; g.port = port; g.bytes.assign(d, d + n); log_.push_back(std::move(g)); }
    return Status::Ok();
  }
  void close() override { std::lock_guard<std::mutex> l(m_); open_ = false; }
  uint64_t datagramsSent() const override { std::lock_guard<std::mutex> l(m_); return sent_; }
  // ---- test helpers
  std::vector<Datagram> records() const { std::lock_guard<std::mutex> l(m_); return log_; }
  void clearRecords() { std::lock_guard<std::mutex> l(m_); log_.clear(); }
  void setFailing(bool f) { std::lock_guard<std::mutex> l(m_); failing_ = f; }
  void setFailOpen(bool f) { std::lock_guard<std::mutex> l(m_); failOpen_ = f; }
  bool isOpen() const { std::lock_guard<std::mutex> l(m_); return open_; }
  bool broadcastAllowed() const { std::lock_guard<std::mutex> l(m_); return broadcast_; }
  std::string nic() const { std::lock_guard<std::mutex> l(m_); return nic_; }
  int opens() const { std::lock_guard<std::mutex> l(m_); return opens_; }
 private:
  mutable std::mutex m_;
  std::vector<Datagram> log_; size_t max_;
  bool open_ = false, failing_ = false, failOpen_ = false, broadcast_ = false; std::string nic_;
  uint64_t sent_ = 0; int opens_ = 0;
};

class CountingSink : public IDatagramSink {
 public:
  Status open(const std::string&, bool) override { open_ = true; return Status::Ok(); }
  Status sendTo(const std::string&, uint16_t, const uint8_t*, size_t n) override {
    if (!open_) return Status::Fail(Err::DeviceLost, "sink not open");
    ++count_; bytes_ += n; return Status::Ok();
  }
  void close() override { open_ = false; }
  uint64_t datagramsSent() const override { return count_; }
  uint64_t bytes() const { return bytes_; }
 private:
  bool open_ = false; uint64_t count_ = 0, bytes_ = 0;
};

class DryRunSink : public IDatagramSink {
 public:
  Status open(const std::string&, bool) override { open_ = true; return Status::Ok(); }
  Status sendTo(const std::string& host, uint16_t port, const uint8_t* d, size_t n) override {
    if (!open_) return Status::Fail(Err::DeviceLost, "dry-run sink not open");
    std::lock_guard<std::mutex> l(m_);
    ++wouldSend_; last_.host = host; last_.port = port; last_.bytes.assign(d, d + n); return Status::Ok();
  }
  void close() override { open_ = false; }
  uint64_t datagramsSent() const override { return 0; }   // by construction: nothing leaves the process
  uint64_t wouldSend() const { std::lock_guard<std::mutex> l(m_); return wouldSend_; }
  Datagram last() const { std::lock_guard<std::mutex> l(m_); return last_; }
 private:
  mutable std::mutex m_; bool open_ = false; uint64_t wouldSend_ = 0; Datagram last_;
};

} // namespace ss
