// Contract: display enumeration with stable identity (app-adapters 4, APP-AD-04/05). Owner: engine-adapters-desktop.
// The provider produces ss::router::PhysicalDisplay records; the router core does the matching. Audience windows
// themselves are UI (OutputManager, ui-live) and consume the router table, not this interface directly.
#pragma once
#include <functional>
#include <vector>
#include "Engine/AdapterCommon.h"
#include "ss/router/router.hpp"

namespace ss::app {

class IDisplayProvider : public IAdapter {
public:
    virtual ss::router::Topology topology() = 0;                       // UI thread
    virtual void setChangeCallback(std::function<void()> cb) = 0;      // UI thread; fired after hot-plug/mode change
};

}  // namespace ss::app
