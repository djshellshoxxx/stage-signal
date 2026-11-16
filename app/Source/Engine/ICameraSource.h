// Contract: camera adapter (app-adapters 14, APP-AD-15). Owner: engine-media-record.
// Preview never implies publication. Frames are copied into a newest-frame mailbox (capacity 2) by the callback.
#pragma once
#include <string>
#include <vector>
#include "Engine/AdapterCommon.h"
#include "ss/base/types.hpp"

namespace ss::app {

enum class CameraState : unsigned char { Off, Previewing, Published, Lost, Unsupported };

struct CameraDesc { std::string id, name; };

class ICameraSink {
public:
    virtual ~ICameraSink() = default;
    virtual void onCameraFrame(const std::string& deviceId, const ss::PixelBuffer& frame, ss::Ns nowNs) = 0;  // any thread; memcpy only
    virtual void onCameraLost(const std::string& deviceId, ss::Ns nowNs) = 0;
};

class ICameraSource : public IAdapter {
public:
    virtual std::vector<CameraDesc> listCameras() = 0;
    virtual ss::Status open(const std::string& id, ICameraSink* sink) = 0;   // starts Previewing, never Published
    virtual void close() = 0;
    virtual CameraState cameraState() const = 0;
};

class NullCameraSource final : public ICameraSource {
public:
    ss::Status start() override { return ss::okStatus(); }
    void stop() override {}
    AdapterHealth health() const override { return {AdapterState::Unavailable, "no camera in this build"}; }
    std::vector<CameraDesc> listCameras() override { return {}; }
    ss::Status open(const std::string&, ICameraSink*) override {
        return ss::makeError(ss::ErrorCategory::UnsupportedCapability, "camera not available");
    }
    void close() override {}
    CameraState cameraState() const override { return CameraState::Unsupported; }
};

}  // namespace ss::app
