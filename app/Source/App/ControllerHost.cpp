#include "App/ControllerHost.h"
#include <mutex>
#include "Engine/DesktopAdapters.h"

namespace ss::app {

std::shared_ptr<AppController> ControllerHost::acquire(HostKind kind) {
    static std::mutex m;
    static std::weak_ptr<AppController> instance;
    std::lock_guard<std::mutex> g(m);
    if (auto c = instance.lock()) return c;
    AppConfig cfg;
    cfg.host = kind;
    auto c = std::make_shared<AppController>(makeDesktopAdapterSet(), cfg);
    c->start();      // failure leaves subsystems Unavailable; the UI shows it from the startup log
    instance = c;
    return c;
}

}  // namespace ss::app
