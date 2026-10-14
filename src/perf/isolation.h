// MOD-14 benchmark isolation (REQ-P02, spec §7 BEN-04/05): scenarios only ever receive disposable sinks. Anything that
// would put bytes on a real network or start a remote broadcast trips the assert-able NetworkTransmissionForbidden guard.
// MOD-14 has no authority to enable transmission; there is deliberately no API here that opens a real socket.
#pragma once
#include "core/types.h"
#include "core/ports.h"
#include <string>
#include <vector>
#include <mutex>

namespace ss {

class NetworkTransmissionForbidden {
 public:
  void report(const std::string& what);          // records a (sanitized) violation
  bool tripped() const { std::lock_guard<std::mutex> g(m_); return n_ > 0; }
  uint64_t count() const { std::lock_guard<std::mutex> g(m_); return n_; }
  std::vector<std::string> events() const { std::lock_guard<std::mutex> g(m_); return ev_; }   // first 16
  void reset() { std::lock_guard<std::mutex> g(m_); n_ = 0; ev_.clear(); }
 private:
  mutable std::mutex m_; uint64_t n_ = 0; std::vector<std::string> ev_;
};

// Lighting: counts and discards. Real NIC / broadcast / non-loopback hosts are refused and trip the guard.
class DisposableDatagramSink : public IDatagramSink {
 public:
  explicit DisposableDatagramSink(NetworkTransmissionForbidden& g) : g_(g) {}
  Status open(const std::string& nicAddr, bool allowBroadcast) override;
  Status sendTo(const std::string& host, uint16_t port, const uint8_t* d, size_t n) override;
  void close() override { open_ = false; }
  uint64_t datagramsSent() const override { return sent_; }
  uint64_t bytes() const { return bytes_; }
 private:
  NetworkTransmissionForbidden& g_; bool open_ = false; uint64_t sent_ = 0, bytes_ = 0;
};

// Recording encode sink: in-memory counting target with a byte budget (simulates a disposable test file).
class DisposableRecordSink {
 public:
  explicit DisposableRecordSink(uint64_t budgetBytes = 64ull << 20) : budget_(budgetBytes) {}
  Status write(const uint8_t* d, size_t n);       // DiskFull once budget exhausted; nothing is stored
  uint64_t bytes() const { return bytes_; }
  uint64_t writes() const { return writes_; }
 private:
  uint64_t budget_, bytes_ = 0, writes_ = 0;
};

// Broadcast encoder/muxer test receiver: accepts only loopback/discard endpoints, otherwise trips the guard.
class LocalDiscardReceiver {
 public:
  explicit LocalDiscardReceiver(NetworkTransmissionForbidden& g) : g_(g) {}
  Status begin(const std::string& endpoint);
  Status sendPacket(const uint8_t* d, size_t n);
  void end() { live_ = false; }
  uint64_t packets() const { return packets_; }
  uint64_t bytes() const { return bytes_; }
 private:
  NetworkTransmissionForbidden& g_; bool live_ = false; uint64_t packets_ = 0, bytes_ = 0;
};

struct BenchmarkSinks {
  NetworkTransmissionForbidden guard;
  DisposableDatagramSink lighting{guard};
  DisposableRecordSink recording;
  LocalDiscardReceiver broadcast{guard};
  // A scenario asking to start a (real) broadcast: always refused, always trips the guard.
  Status requestRemoteBroadcast(const std::string& endpointLabel);
};

} // namespace ss
