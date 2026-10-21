// Approved overlays (section 11/13): BPM/key labels with Manual/Estimated marker, branding, optional diagnostics.
// Policy is per output. Default OutputPolicy shows text only; audience outputs never get diagnostics unless the
// route explicitly enables them (REQ-D05). The overlay data model has no filesystem-path or secret fields, and the
// diagnostics lines are additionally redacted when they look like a path or a secret.
#pragma once
#include <array>
#include <memory>
#include "ss/base/types.hpp"
#include "ss/compose/limits.hpp"
#include "ss/compose/pixel.hpp"

namespace ss::compose {

struct OutputPolicy {
    bool showText = true;
    bool showBpmKey = false;
    bool showBranding = false;
    bool showDiagnostics = false;   // also gates the clip warning
};
OutputPolicy policyFromRoute(const ss::OutputRoute& r);

struct OverlayInfo {
    ss::TempoState tempo;
    ss::KeyState key;
    bool clipWarning = false;                       // "possible input clipping"
    std::shared_ptr<const PixelBuffer> brandingLogo;  // approved branding bitmap, premultiplied
    std::array<char, 32> brandingText{};            // ASCII, NUL terminated
    std::array<std::array<char, Limits::kMaxOverlayLineChars>, Limits::kMaxOverlayLines> diagnostics{};
    int diagnosticCount = 0;
    void setBrandingText(const char* s);
    void addDiagnostic(const char* s);              // ignored when full
};

// Label text into a caller buffer (no allocation). Returns false when the value is not valid (Unknown source or
// bpm <= 0): an unknown tempo is never rendered as a number. Examples: "BPM 128 (Manual)", "BPM 127.5 (Estimated)",
// "Key Am (Estimated)", "BPM 120 (Tap)".
bool formatTempoLabel(const ss::TempoState& t, char* out, std::size_t cap);
bool formatKeyLabel(const ss::KeyState& k, char* out, std::size_t cap);
bool looksSensitive(const char* line);              // path- or secret-like text

// Draw all overlays the policy allows onto the (logical) composition canvas.
void drawOverlays(PixelBuffer& canvas, const OutputPolicy& policy, const OverlayInfo& info);

}  // namespace ss::compose
