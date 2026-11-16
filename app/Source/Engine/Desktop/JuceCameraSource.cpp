#include "Engine/Desktop/JuceCameraSource.h"
#include <atomic>
#include <mutex>
#include <juce_core/juce_core.h>
#if JUCE_MAC || JUCE_WINDOWS
#include <juce_video/juce_video.h>
#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>
#endif

namespace ss::app {
namespace {
void ensureCameraApartment() {
#if JUCE_WINDOWS
    struct Apartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    };
    // Device commands and MEDIA polling occur on worker threads, outside JUCE's GUI COM apartment.
    thread_local Apartment apartment;
    (void) apartment;
#endif
}
class JuceCameraSource final : public ICameraSource, private juce::CameraDevice::Listener {
public:
    explicit JuceCameraSource(std::shared_ptr<ss::IClock> clock) : clock_(std::move(clock)) {}
    ~JuceCameraSource() override { close(); }
    ss::Status start() override { return ss::okStatus(); }
    void stop() override { close(); }
    AdapterHealth health() const override {
        return {lost_.load() ? AdapterState::Fault : (deviceOpen_.load() ? AdapterState::Running : AdapterState::Stopped),
                lost_.load() ? "Camera unavailable or permission denied. Select it again to retry." : ""};
    }
    std::vector<CameraDesc> listCameras() override {
        ensureCameraApartment();
        std::vector<CameraDesc> out;
        const auto names = juce::CameraDevice::getAvailableDevices();
        for (const auto& name : names) out.push_back({name.toStdString(), name.toStdString()});
        return out;
    }
    ss::Status open(const std::string& id, ICameraSink* sink) override {
        ensureCameraApartment();
        std::lock_guard<std::recursive_mutex> control(controlMutex_);
        close();
        if (!sink || id.empty()) return ss::makeError(ss::ErrorCategory::InvalidArgument, "Choose a camera first");
        const auto names = juce::CameraDevice::getAvailableDevices();
        int index = -1;
        for (int i = 0; i < names.size(); ++i) {
            if (names[i].toStdString() != id) continue;
            if (index >= 0) return ss::makeError(ss::ErrorCategory::InvalidArgument, "Camera names are ambiguous; rename or disconnect duplicate cameras");
            index = i;
        }
        if (index < 0) return ss::makeError(ss::ErrorCategory::DeviceLost, "The selected camera disconnected; no other camera was opened");
        device_.reset(juce::CameraDevice::openDevice(index, 128, 64, 1280, 720, false));
        if (!device_) return ss::makeError(ss::ErrorCategory::UnsupportedCapability, "Camera could not open; check operating-system camera permission");
        id_ = id; sink_ = sink; lost_.store(false); deviceOpen_.store(true);
        device_->onErrorOccurred = [this](const juce::String&) { lost_.store(true); };
        device_->addListener(this);
        return ss::okStatus();
    }
    void close() override {
        ensureCameraApartment();
        std::lock_guard<std::recursive_mutex> control(controlMutex_);
        deviceOpen_.store(false);
        if (device_) { device_->removeListener(this); device_->onErrorOccurred = {}; device_.reset(); }
        std::lock_guard<std::mutex> lock(mailboxMutex_);
        newest_ = {}; sink_ = nullptr; id_.clear(); lost_.store(false);
    }
    CameraState cameraState() const override {
        return lost_.load() ? CameraState::Lost : (deviceOpen_.load() ? CameraState::Previewing : CameraState::Off);
    }
    // MEDIA thread. Pixel conversion/resize and delivery happen here, never in the device callback.
    void poll() override {
        ensureCameraApartment();
        std::lock_guard<std::recursive_mutex> control(controlMutex_);
        if (!deviceOpen_.load() || !sink_) return;
        if (lost_.load()) {
            sink_->onCameraLost(id_, now());
            return;
        }
        juce::Image image;
        { std::lock_guard<std::mutex> lock(mailboxMutex_); std::swap(image, newest_); }
        if (image.isNull()) return;
        if (image.getWidth() > 1280 || image.getHeight() > 720) {
            const double scale = std::min(1280. / image.getWidth(), 720. / image.getHeight());
            image = image.rescaled(std::max(1, int(image.getWidth() * scale)), std::max(1, int(image.getHeight() * scale)));
        }
        pixels_.width = static_cast<std::uint32_t>(image.getWidth());
        pixels_.height = static_cast<std::uint32_t>(image.getHeight());
        pixels_.rgba.resize(std::size_t(pixels_.width) * pixels_.height * 4);
        juce::Image::BitmapData read(image, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < image.getHeight(); ++y) for (int x = 0; x < image.getWidth(); ++x) {
            const auto colour = read.getPixelColour(x, y);
            const auto at = (std::size_t(y) * pixels_.width + std::size_t(x)) * 4;
            pixels_.rgba[at] = colour.getRed(); pixels_.rgba[at + 1] = colour.getGreen();
            pixels_.rgba[at + 2] = colour.getBlue(); pixels_.rgba[at + 3] = 255;
        }
        sink_->onCameraFrame(id_, pixels_, now());
    }
private:
    ss::Ns now() const { return clock_->nowMonotonic(); }
    void imageReceived(const juce::Image& image) override {
        std::unique_lock<std::mutex> lock(mailboxMutex_, std::try_to_lock);
        if (lock.owns_lock()) newest_ = image;  // reference copy only; drop a frame instead of blocking capture
    }
    std::shared_ptr<ss::IClock> clock_;
    std::unique_ptr<juce::CameraDevice> device_;
    std::atomic<bool> deviceOpen_{false}, lost_{false};
    std::recursive_mutex controlMutex_;
    std::mutex mailboxMutex_;
    juce::Image newest_;
    ICameraSink* sink_ = nullptr;
    std::string id_;
    ss::PixelBuffer pixels_;
};
}  // namespace
std::shared_ptr<ICameraSource> makeJuceCameraSource(std::shared_ptr<ss::IClock> clock) { return std::make_shared<JuceCameraSource>(std::move(clock)); }
}  // namespace ss::app
#else
namespace ss::app {
std::shared_ptr<ICameraSource> makeJuceCameraSource(std::shared_ptr<ss::IClock>) { return std::make_shared<NullCameraSource>(); }
}
#endif
