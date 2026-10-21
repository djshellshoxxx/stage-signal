// Lighting limits (spec sections 14/17). Bounds are checked before any allocation or enqueue.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ss::lighting {

constexpr std::size_t kDmxSlots = 512;
constexpr int kDefaultRateHz = 25;
constexpr int kMaxRateHz = 40;                    // maximum qualified lighting rate
constexpr std::uint16_t kArtNetPort = 6454;       // 0x1936
constexpr std::uint16_t kSacnPort = 5568;
constexpr std::uint8_t kArtNetProtocolVersion = 14;
constexpr std::uint16_t kArtNetMaxPortAddress = 32767;   // 15 bit Port-Address
constexpr std::uint16_t kSacnMinUniverse = 1, kSacnMaxUniverse = 63999;
constexpr std::uint8_t kSacnDefaultPriority = 100;
constexpr std::size_t kMaxUniverses = 4;          // enhanced provisional cap (spec section 8, DMX baseline)
constexpr std::size_t kMaxProfileChannels = 64;
constexpr std::size_t kMaxProfiles = 64;
constexpr std::size_t kMaxFixtures = 256;         // matches model::Limits::kMaxFixtures
constexpr std::size_t kMaxLooks = 256;
constexpr std::size_t kMaxAudioMappings = 128;
constexpr std::size_t kMaxEvents = 64;            // bounded status event log
constexpr int kTerminationRepeats = 3;            // E1.31 stream-terminated packets
constexpr int kSafeFrameRepeats = 3;              // UDP is lossy; graceful stop repeats safe frames

}  // namespace ss::lighting
