// UI-facing lighting status. Struct owner: engine-adapters-desktop (extend fields freely; keep it copyable).
// Producer: LightingScheduler::fillView(). Consumers: ui-other (MIDI/Lighting tab), ui-live (status chips).
#pragma once
#include <cstdint>
#include <string>

namespace ss::app {

enum class LightingUiState : unsigned char { Disarmed, Armed, DryRun, Fault };

struct LightingView {
    LightingUiState state = LightingUiState::Disarmed;
    bool transportConnected = false;
    bool blackout = false;
    std::uint64_t ticks = 0, framesSent = 0, ticksHeldDisarmed = 0;
    std::string note;           // sanitised; e.g. "Disarmed, press Arm"
};

}  // namespace ss::app
