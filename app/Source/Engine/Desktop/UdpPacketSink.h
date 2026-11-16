// IPacketSink: native non-blocking UDP sender for Art-Net/sACN (app-adapters 6.1, APP-AD-07). Native sockets are used
// because juce::DatagramSocket cannot set the multicast interface or TTL. Nothing is bound until bindInterface();
// connected() is false before that, so LightingScheduler::arm() is refused ("no lighting transport").
// send() is for the LIGHT thread only and never blocks (EWOULDBLOCK counts as a drop and is reported as a failure).
#pragma once
#include <atomic>
#include <mutex>
#include "Engine/IPacketSink.h"

namespace ss::app {

class UdpPacketSink final : public IPacketSink {
public:
    UdpPacketSink();
    ~UdpPacketSink() override;

    ss::Status start() override;
    void stop() override;
    AdapterHealth health() const override;
    std::vector<NetworkInterfaceDesc> listInterfaces() override;
    ss::Status bindInterface(const std::string& ipv4) override;      // closes and re-opens; bumps interfaceGeneration()
    ss::Status send(const ss::lighting::Datagram& d) override;
    bool connected() const override { return connected_.load(std::memory_order_acquire); }

    void setBroadcastEnabled(bool on);                               // SO_BROADCAST only when explicitly enabled
    void setMulticastTtl(int ttl);                                   // default 4
    std::uint64_t interfaceGeneration() const { return ifaceGen_.load(); }
    std::uint64_t sentCount() const { return sent_.load(); }
    std::uint64_t failedCount() const { return failed_.load(); }

private:
    ss::Status openLocked(const std::string& ipv4);
    void closeLocked();
    void fault(const std::string& why);

    mutable std::mutex m_;                 // lifecycle + health text; send() does not take it
    std::atomic<std::intptr_t> sock_{-1};
    std::atomic<bool> connected_{false}, started_{false}, broadcast_{false};
    std::atomic<int> ttl_{4};
    std::atomic<std::uint64_t> ifaceGen_{0}, sent_{0}, failed_{0};
    std::string boundIp_, lastError_;
    AdapterState state_ = AdapterState::Stopped;
};

}  // namespace ss::app
