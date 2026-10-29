// Display provider and file dialogs: both use JUCE GUI classes and must be used on the message thread.
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine/Desktop/DesktopCommon.h"
#include "Engine/Desktop/DesktopSystem.h"
#include "Engine/Desktop/NativeDisplayIdentity.h"

namespace ss::app {

struct JuceDisplayProvider::Impl final : juce::Timer {
    std::function<void()> cb;
    bool started = false;
    std::string lastSig;
    ss::router::Topology cur;

    static std::string hashId(const juce::Displays::Display& d) {
        // JUCE exposes no EDID/UUID: identity = hash of physical mode + scale. Identical monitors collide by design;
        // the router flags those as indistinguishable (APP-AD-04).
        const juce::String s = juce::String(d.physicalBounds.getWidth()) + "x" + juce::String(d.physicalBounds.getHeight()) + "@"
                             + juce::String(d.scale, 3) + "/" + juce::String(d.dpi, 0);
        return "juce:" + juce::String::toHexString(s.hashCode64()).toStdString();
    }

    ss::router::Topology read() const {
        ss::router::Topology t;
        const auto& ds = juce::Desktop::getInstance().getDisplays();
        const auto native = nativeDisplayIdentities();
        int i = 0;
        for (const auto& d : ds.displays) {
            ss::router::PhysicalDisplay p;
            p.identity = hashId(d);
            p.connector = "display" + std::to_string(i++) + "@" + std::to_string(d.physicalBounds.getX()) + "," + std::to_string(d.physicalBounds.getY());
#if JUCE_MAC
            const auto nativeBounds = d.logicalBounds.toNearestInt();
#else
            const auto nativeBounds = d.physicalBounds;
#endif
            if (const auto* record = uniqueNativeDisplay(native, nativeBounds.getX(), nativeBounds.getY(), nativeBounds.getWidth(), nativeBounds.getHeight())) {
                p.identity = record->identity; p.connector = record->connector;
            }
            p.width = d.physicalBounds.getWidth(); p.height = d.physicalBounds.getHeight();
            p.dpiScale = d.scale; p.primary = d.isMain;
            t.push_back(std::move(p));
        }
        return t;
    }
    static std::string sig(const ss::router::Topology& t) {
        std::string s;
        for (const auto& p : t) s += p.connector + "|" + p.identity + "|" + std::to_string(p.width) + "x" + std::to_string(p.height) + ";";
        return s;
    }
    void timerCallback() override {
        auto t = read();
        const auto sg = sig(t);
        if (sg != lastSig) { lastSig = sg; cur = std::move(t); if (cb) cb(); }
    }
};

JuceDisplayProvider::JuceDisplayProvider() : impl_(std::make_unique<Impl>()) {}
JuceDisplayProvider::~JuceDisplayProvider() { stop(); }

ss::Status JuceDisplayProvider::start() {
    if (impl_->started) return ss::okStatus();
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
        return desktop::err(ss::ErrorCategory::UnsupportedCapability, "no message loop for display polling");
    impl_->started = true;
    impl_->cur = impl_->read(); impl_->lastSig = Impl::sig(impl_->cur);
    impl_->startTimer(1000);                         // JUCE has no display-change listener: 1 s poll (spec 4.3 fallback)
    return ss::okStatus();
}

void JuceDisplayProvider::stop() { impl_->stopTimer(); impl_->started = false; }

AdapterHealth JuceDisplayProvider::health() const {
    if (!impl_->started) return {AdapterState::Stopped, {}};
    for (const auto& display : impl_->cur)
        if (display.identity.rfind("juce:", 0) == 0) return {AdapterState::Degraded, "native display identity unavailable or ambiguous; confirm role explicitly"};
    return {AdapterState::Running, {}};
}

ss::router::Topology JuceDisplayProvider::topology() {
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr) return {};
    impl_->cur = impl_->read();
    return impl_->cur;
}

void JuceDisplayProvider::setChangeCallback(std::function<void()> cb) { impl_->cb = std::move(cb); }

// ---------------------------------------------------------------------------------------------------------------------
void JuceFileDialogs::launch(DialogKind kind, const std::string& startDir, std::function<void(const std::string&)> done) {
    using Flags = juce::FileBrowserComponent;
    int flags = Flags::canSelectFiles;
    juce::String title = "Open", pattern = "*";
    switch (kind) {
        case DialogKind::OpenShow:    title = "Open show";    pattern = "*.stagesignal;*.json"; flags |= Flags::openMode; break;
        case DialogKind::SaveShow:    title = "Save show";    pattern = "*.stagesignal";        flags = Flags::saveMode | Flags::canSelectFiles | Flags::warnAboutOverwriting; break;
        case DialogKind::ChooseFolder: title = "Choose folder"; flags = Flags::openMode | Flags::canSelectDirectories; break;
        case DialogKind::OpenMedia:   title = "Open media";   pattern = "*.png;*.jpg;*.jpeg;*.mp4;*.mov;*.mkv;*.wav"; flags |= Flags::openMode; break;
        case DialogKind::SaveReport:  title = "Save report";  pattern = "*.zip;*.json;*.txt";   flags = Flags::saveMode | Flags::canSelectFiles | Flags::warnAboutOverwriting; break;
    }
    auto chooser = std::make_shared<juce::FileChooser>(title, juce::File(juce::String::fromUTF8(startDir.c_str())), pattern, true);
    chooser->launchAsync(flags, [chooser, done = std::move(done)](const juce::FileChooser& fc) {
        const auto r = fc.getResult();                                    // empty File on cancel
        done(r == juce::File() ? std::string() : r.getFullPathName().toStdString());
    });
}

}  // namespace ss::app
