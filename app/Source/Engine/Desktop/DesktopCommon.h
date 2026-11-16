// Shared helpers for the real desktop adapters (engine-adapters-desktop). Header-only, no JUCE dependency.
#pragma once
#include <string>
#include "ss/base/error.hpp"
#include "ss/security/redact.hpp"

namespace ss::app::desktop {

// Health/error text that may reach the UI or logs: redacted, single line, bounded. Never put paths in it.
inline std::string clean(const std::string& s) { return ss::security::sanitizeForLog(s, 160); }

inline ss::Error err(ss::ErrorCategory c, const std::string& what,
                     ss::Recoverability r = ss::Recoverability::OperatorAction) {
    return ss::makeError(c, clean(what), r);
}

}  // namespace ss::app::desktop
