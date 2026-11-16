// Contract: common adapter lifecycle (app-adapters section 2). Owner of this file: shell-integration.
// Adapters convert one platform facility into a core interface. They hold no show logic, never call UI,
// and report state changes through health(). start()/stop() are idempotent and bounded in time.
#pragma once
#include <string>
#include "ss/base/error.hpp"

namespace ss::app {

enum class AdapterState : unsigned char { Stopped, Starting, Running, Degraded, Fault, Unavailable };

inline const char* adapterStateName(AdapterState s) {
    switch (s) {
        case AdapterState::Stopped: return "Stopped";
        case AdapterState::Starting: return "Starting";
        case AdapterState::Running: return "Running";
        case AdapterState::Degraded: return "Degraded";
        case AdapterState::Fault: return "Fault";
        case AdapterState::Unavailable: return "Unavailable";
    }
    return "Unknown";
}

struct AdapterHealth {
    AdapterState state = AdapterState::Stopped;
    std::string lastError;     // sanitised (no secrets, no absolute user paths); empty when none
};

class IAdapter {
public:
    virtual ~IAdapter() = default;
    virtual ss::Status start() = 0;             // idempotent; unsupported platforms return UnsupportedCapability
    virtual void stop() = 0;                    // idempotent; bounded; never throws
    virtual AdapterHealth health() const = 0;   // any thread
};

}  // namespace ss::app
