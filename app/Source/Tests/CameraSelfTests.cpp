#include "App/Media/CameraFeed.h"
#include "Tests/SelfTest.h"

namespace ss::app {
namespace {
class ScriptedCamera final : public ICameraSource {
public:
    ss::Status start() override { return ss::okStatus(); }
    void stop() override { close(); }
    AdapterHealth health() const override { return {AdapterState::Running, {}}; }
    std::vector<CameraDesc> listCameras() override { return {{"selected", "Selected"}}; }
    ss::Status open(const std::string& id, ICameraSink* next) override {
        if (id != "selected") return ss::makeError(ss::ErrorCategory::DeviceLost, "No selected camera");
        sink = next; return ss::okStatus();
    }
    void close() override { sink = nullptr; }
    CameraState cameraState() const override { return sink ? CameraState::Previewing : CameraState::Off; }
    ICameraSink* sink = nullptr;
};
SelfTestResult cameraPrivacy() {
    auto source = std::make_shared<ScriptedCamera>();
    CameraFeed feed(source);
    if (feed.publish(1).ok()) return {false, "unopened camera published"};
    if (!feed.open("selected").ok()) return {false, "selected camera did not open"};
    if (feed.publish(1).ok()) return {false, "camera published before preview existed"};
    PixelBuffer frame(8, 8);
    std::fill(frame.rgba.begin(), frame.rgba.end(), 255);
    feed.onCameraFrame("foreign", frame, secToNs(1));
    if (feed.preview()) return {false, "foreign camera became preview"};
    feed.onCameraFrame("selected", frame, secToNs(1));
    if (!feed.preview()) return {false, "selected preview missing"};
    ss::compose::FrameInputs in;
    feed.fillLayers(1, secToNs(1), in);
    if (!in.camera || in.camera->visible()) return {false, "preview leaked to audience"};
    if (!feed.publish(1).ok()) return {false, "explicit publication rejected"};
    feed.fillLayers(2, secToNs(1), in);
    if (in.camera->visible()) return {false, "camera leaked to another output"};
    feed.fillLayers(1, secToNs(1), in);
    if (!in.camera->visible()) return {false, "published camera missing"};
    feed.onCameraLost("selected", secToNs(1));
    feed.fillLayers(1, secToNs(1) + msToNs(501), in);
    if (in.camera->visible()) return {false, "lost camera visible after freeze budget"};
    feed.unpublish();
    feed.onCameraFrame("selected", frame, secToNs(2));
    feed.fillLayers(1, secToNs(2), in);
    if (in.camera->visible()) return {false, "unpublished camera automatically republished"};
    feed.close();
    if (feed.preview()) return {false, "closed camera kept preview"};
    if (feed.open("foreign").ok() || feed.preview()) return {false, "missing camera silently fell back"};
    return {true, "preview/publish isolation, target routing, source binding, loss budget and close"};
}
}  // namespace
void registerCameraSelfTests(SelfTestRegistry& r) { r.add("camera: publication privacy and loss", cameraPrivacy); }
}  // namespace ss::app
