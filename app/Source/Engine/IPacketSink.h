// Contract: UDP transport for Art-Net/sACN (app-adapters 6, APP-AD-07). Owner: engine-adapters-desktop.
// Reuses ss::lighting::IDatagramSink so a LightingEngine can be handed the adapter directly.
// Nothing is sent before Arm: the LIGHT thread is given NullPacketSink while disarmed (app-shell 5 step 11).
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "Engine/AdapterCommon.h"
#include "ss/lighting/sink.hpp"

namespace ss::app {

struct NetworkInterfaceDesc { std::string name, ipv4; bool up = false; };

class IPacketSink : public IAdapter, public ss::lighting::IDatagramSink {
public:
    virtual std::vector<NetworkInterfaceDesc> listInterfaces() = 0;
    virtual ss::Status bindInterface(const std::string& ipv4) = 0;   // changing it forces a disarm (REQ-L05)
    // send() and connected() come from IDatagramSink: LIGHT thread only, non-blocking.
};

// Drops everything and counts it. Used while disarmed and in tests.
class NullPacketSink final : public IPacketSink {
public:
    ss::Status start() override { return ss::okStatus(); }
    void stop() override {}
    AdapterHealth health() const override { return {AdapterState::Running, {}}; }
    std::vector<NetworkInterfaceDesc> listInterfaces() override { return {}; }
    ss::Status bindInterface(const std::string&) override { return ss::okStatus(); }
    ss::Status send(const ss::lighting::Datagram&) override { ++dropped; return ss::okStatus(); }
    bool connected() const override { return false; }
    std::uint64_t dropped = 0;
};

// Records what would have been transmitted. Tests assert "nothing sent while disarmed".
class RecordingPacketSink final : public IPacketSink {
public:
    ss::Status start() override { return ss::okStatus(); }
    void stop() override {}
    AdapterHealth health() const override { return {AdapterState::Running, {}}; }
    std::vector<NetworkInterfaceDesc> listInterfaces() override { return {{"fake0", "127.0.0.1", true}}; }
    ss::Status bindInterface(const std::string&) override { return ss::okStatus(); }
    ss::Status send(const ss::lighting::Datagram&) override { sent.fetch_add(1); return ss::okStatus(); }
    bool connected() const override { return true; }
    std::atomic<std::uint64_t> sent{0};
};

}  // namespace ss::app
