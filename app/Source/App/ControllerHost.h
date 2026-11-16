// Process-wide AppController holder (app-shell 3: "lives in a process-wide shared_ptr"). Owner: shell-integration.
// Plugin mode: the first prepareToPlay or createEditor creates and starts it; it stops when the last holder
// (processor or editor) lets go. Several plugin instances in one host currently SHARE it; the "Secondary
// instance" mode of app-shell 7.3 is a shell-integration follow-up. Standalone uses its own instance owned by
// StageSignalApp and never goes through here.
#pragma once
#include <memory>
#include "App/AppController.h"

namespace ss::app {

class ControllerHost {
public:
    // Creates (with makeDesktopAdapterSet()) and starts the controller on first use. Never returns null.
    static std::shared_ptr<AppController> acquire(HostKind kind);
};

}  // namespace ss::app
