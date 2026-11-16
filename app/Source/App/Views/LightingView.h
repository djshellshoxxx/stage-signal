// UI-facing lighting status. Struct owner: engine-adapters-desktop (extend fields freely; keep it copyable).
// Producer: LightingScheduler::fillView(). Consumers: ui-other (MIDI/Lighting tab), ui-live (status chips).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ss::app {

enum class LightingUiState : unsigned char { Disarmed, Armed, DryRun, Fault };

struct LightingInterfaceRow { std::string name, ipv4; bool up = false; };

struct LightingView {
    LightingUiState state = LightingUiState::Disarmed;
    bool transportConnected = false;
    bool blackout = false;
    bool engineArmed = false;           // the core LightingEngine's own state (follows the scheduler gate)
    std::uint64_t ticks = 0, framesSent = 0, ticksHeldDisarmed = 0;
    std::uint64_t datagramsReal = 0, datagramsMock = 0, faults = 0;   // real = left the process; mock = dry/preview only
    std::string interfaceIp;            // the interface the app last bound ("" = none chosen)
    std::string destination;            // unicast IPv4 from the show ("" = none)
    int port = 0;                       // wire port of the protocol (6454 Art-Net, 5568 sACN)
    std::string protocol;               // "Art-Net" or "sACN"
    bool broadcast = false;             // Art-Net directed broadcast opt-in
    int universes = 0, fixtures = 0;
    std::vector<std::string> patchIssues;   // one sanitised line per problem, empty when the patch is clean
    bool patchCanArm = true;
    std::vector<LightingInterfaceRow> interfaces;
    std::string note;           // sanitised; e.g. "Disarmed, press Arm"
};

}  // namespace ss::app
