// Contract: optional USB-DMX serial adapter (app-adapters 7). Owner: engine-adapters-desktop.
// Opening requires Arm; no DMX512 timing claim is made. Device loss implies Fault + Disarm.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Engine/AdapterCommon.h"

namespace ss::app {

struct SerialPortInfo { std::string path, description; bool qualified = false; };   // qualified only after a physical test

class ISerialDmx : public IAdapter {
public:
    virtual std::vector<SerialPortInfo> listPorts() = 0;
    virtual ss::Status open(const std::string& path) = 0;
    virtual ss::Status sendUniverse(const std::uint8_t* data512, std::uint32_t n) = 0;   // LIGHT thread
};

class NullSerialDmx final : public ISerialDmx {
public:
    ss::Status start() override { return ss::okStatus(); }
    void stop() override {}
    AdapterHealth health() const override { return {AdapterState::Unavailable, "no serial adapter in this build"}; }
    std::vector<SerialPortInfo> listPorts() override { return {}; }
    ss::Status open(const std::string&) override {
        return ss::makeError(ss::ErrorCategory::UnsupportedCapability, "serial DMX not available");
    }
    ss::Status sendUniverse(const std::uint8_t*, std::uint32_t) override { return ss::okStatus(); }
};

}  // namespace ss::app
