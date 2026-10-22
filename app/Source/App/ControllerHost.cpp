#include "App/ControllerHost.h"
#include <mutex>
#include <cstdlib>
#include <juce_core/juce_core.h>
#include "Engine/DesktopAdapters.h"
#include "Engine/Fakes.h"

namespace ss::app {

std::shared_ptr<AppController> ControllerHost::acquire(HostKind kind) {
    static std::mutex m;
    static std::weak_ptr<AppController> primary, secondary;
    std::lock_guard<std::mutex> g(m);
    AppConfig cfg;
    cfg.host = kind;
    if (primary.lock()) {
        // Section 7.3: a second instance in the same process runs analysis and UI only. Fake adapters mean it touches
        // no device, socket or file; every output action is refused with a stated reason.
        if (auto c = secondary.lock()) return c;
        cfg.secondary = true;
        cfg.startAudio = false;
        auto c = std::make_shared<AppController>(makeFakeAdapterSet(false), cfg);
        c->start();
        secondary = c;
        return c;
    }
    if (const char* d = std::getenv("SS_DATA_DIR")) cfg.dataDir = d;
    else cfg.dataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("StageSignal").getFullPathName().toStdString();
    auto c = std::make_shared<AppController>(makeDesktopAdapterSet(), cfg);
    c->start();      // failure leaves subsystems Unavailable; the UI shows it from the startup log
    primary = c;
    return c;
}

}  // namespace ss::app
