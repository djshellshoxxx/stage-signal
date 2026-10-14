#include "perf/isolation.h"
#include "core/security.h"

namespace ss {

void NetworkTransmissionForbidden::report(const std::string& what) {
  std::lock_guard<std::mutex> g(m_); ++n_; if (ev_.size() < 16) ev_.push_back(sec::sanitizeLibraryError(what, 160));
}

Status DisposableDatagramSink::open(const std::string& nic, bool allowBroadcast) {
  if (!nic.empty() || allowBroadcast) {
    g_.report("lighting sink open with real NIC/broadcast in benchmark");
    return Status::Fail(Err::PermissionDenied, "benchmark lighting sink is disposable: no NIC/broadcast", false);
  }
  open_ = true; return Status::Ok();
}
Status DisposableDatagramSink::sendTo(const std::string& host, uint16_t port, const uint8_t* d, size_t n) {
  if (!open_) return Status::Fail(Err::InvalidArgument, "sink not open");
  if (!sec::isLoopbackOrDiscardEndpoint(host)) {
    g_.report("lighting datagram to non-loopback host: " + host);
    return Status::Fail(Err::PermissionDenied, "benchmark lighting sink refuses non-loopback destination", false);
  }
  (void)port; (void)d; ++sent_; bytes_ += n; return Status::Ok();
}

Status DisposableRecordSink::write(const uint8_t* d, size_t n) {
  (void)d; if (bytes_ + n > budget_) return Status::Fail(Err::DiskFull, "disposable recording target full");
  bytes_ += n; ++writes_; return Status::Ok();
}

Status LocalDiscardReceiver::begin(const std::string& endpoint) {
  if (!sec::isLoopbackOrDiscardEndpoint(endpoint)) {
    g_.report("broadcast begin to non-local endpoint: " + endpoint);
    return Status::Fail(Err::PermissionDenied, "benchmark broadcast target must be a local discard/test receiver", false);
  }
  live_ = true; return Status::Ok();
}
Status LocalDiscardReceiver::sendPacket(const uint8_t* d, size_t n) {
  (void)d; if (!live_) return Status::Fail(Err::InvalidArgument, "receiver not begun");
  ++packets_; bytes_ += n; return Status::Ok();
}

Status BenchmarkSinks::requestRemoteBroadcast(const std::string& label) {
  guard.report("broadcast start requested during benchmark: " + label);
  return Status::Fail(Err::PermissionDenied, "benchmark never starts a broadcast (REQ-P02)", false);
}

} // namespace ss
