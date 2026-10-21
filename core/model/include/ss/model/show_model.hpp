// StageSignal model (MOD-13 pure data, section 17): the portable show document. No I/O, no JSON here
// (serialisation lives in `persist`). Limits are enforced by `persist` before allocation.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/types.hpp"

namespace ss::model {

constexpr std::uint32_t kSchemaMajor = 1, kSchemaMinor = 0;

// Provisional import limits (spec section 17).
struct Limits {
    static constexpr std::size_t kMaxJsonBytes = 8u << 20, kMaxCues = 10000, kMaxScenes = 1000, kMaxMessages = 256,
        kMaxMidiMappings = 512, kMaxFixtures = 256, kMaxArchiveEntries = 10000, kMaxTextCodePoints = 4096;
    static constexpr std::uint64_t kMaxDecompressedBytes = 2ull << 30;
};

enum class SceneKind : std::uint8_t {
    SpectrumBars, MirrorBars, WaveformRibbon, GeometricOrbit, RadialPulse, ParticleField,
    NeonGrid, Tunnel, ColorField, Starburst, PlasmaLite, LissajousScope,
    Image, Gif, Video, Count
};
enum class EffectKind : std::uint8_t { None, PrismMirror, BloomPulse, Count };
enum class EnergyTier : std::uint8_t { Any, Low, Medium, High, Drop };   // Energy Arc pool tagging
enum class TransitionKind : std::uint8_t { Cut, Crossfade, FadeThroughBlack };
enum class TimeBase : std::uint8_t { Seconds, Beats };

struct Rgb { std::uint8_t r = 255, g = 255, b = 255; bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; } };

struct SceneParams {
    float brightness = 1.f, speed = 1.f, intensity = 0.6f, hue = 0.f, saturation = 0.8f;
    float effectAmount[2] = {0.f, 0.f};        // PrismMirror, BloomPulse (0 = off)
    int prismSegments = 6;                      // 2..16
    bool operator==(const SceneParams& o) const {
        return brightness == o.brightness && speed == o.speed && intensity == o.intensity && hue == o.hue &&
               saturation == o.saturation && effectAmount[0] == o.effectAmount[0] &&
               effectAmount[1] == o.effectAmount[1] && prismSegments == o.prismSegments;
    }
};

struct SceneDef {
    std::uint32_t id = 0;
    std::string name;
    SceneKind kind = SceneKind::SpectrumBars;
    SceneParams params;
    std::uint64_t seed = 1;
    bool strobe = false;                        // scene contains flash/strobe content (REQ-S12)
    float costEstimate = 1.f;                   // relative CPU cost, used by admission
    EnergyTier tier = EnergyTier::Any;
    bool inAutoPool = true;
    std::uint32_t mediaId = 0;                  // for Image/Gif/Video scenes
};

struct CueDef {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t sceneId = 0;
    double duration = 8.0;                      // in `timeBase` units; 0 = hold until next trigger
    TimeBase timeBase = TimeBase::Seconds;
    TransitionKind transition = TransitionKind::Crossfade;
    double transitionDuration = 1.0;            // seconds
    std::uint32_t mediaId = 0, textId = 0;
    std::vector<std::uint32_t> lightingRefs;    // fixture group / look ids
    bool waitForTrigger = false;                // band mode: wait for foot-controller Next
};

struct OutputDef {
    OutputId id = kPrimaryOutput;
    std::string label = "Main";
    RouteKind kind = RouteKind::Audience;
    OutputId duplicateOf = 0;
    bool showBpmKey = false, showText = true, showBranding = false;
    int width = 800, height = 600;
    std::string matchHint;                      // optional non-secret display matching hint
};

struct MediaRef {
    std::uint32_t id = 0;
    std::string relativePath;                   // inside the show bundle, never absolute
    std::string sha256;
    std::string format;                         // "png","jpg","bmp","gif","mp4","mkv"...
    std::uint32_t width = 0, height = 0;
    std::uint64_t byteSize = 0;
    double durationSec = 0;
    bool flashRisk = false;
};

struct TextMessage {
    std::uint32_t id = 0;
    std::string utf8;                           // <= 4096 code points
    float x = 0.5f, y = 0.5f, size = 0.12f;     // normalised
    Rgb color;
    double durationSec = 5.0, fadeSec = 0.5;
    bool scroll = false;
};

enum class MidiMsgType : std::uint8_t { Note, ControlChange, ProgramChange };
enum class EncoderMode : std::uint8_t { Absolute, RelativeTwosComplement, RelativeBinaryOffset, RelativeSignBit };
struct MidiMapping {
    std::uint32_t id = 0;
    std::string deviceName;                     // fallback matching; stable identity preferred
    std::string deviceIdentity;
    std::uint8_t channel = 0;                   // 0..15, 255 = omni
    MidiMsgType type = MidiMsgType::Note;
    std::uint8_t number = 0;
    std::uint8_t valueMin = 0, valueMax = 127;
    EncoderMode encoder = EncoderMode::Absolute;
    std::uint32_t actionId = 0;                 // ss::ActionId value (stable numeric id)
};

enum class ChannelFunction : std::uint8_t { Intensity, Red, Green, Blue, White, Strobe, Pan, Tilt, Macro, Other };
struct FixtureChannel {
    ChannelFunction function = ChannelFunction::Intensity;
    std::uint8_t byteWidth = 1;                 // 1 or 2 (coarse/fine)
    std::uint8_t legalMin = 0, legalMax = 255, defaultValue = 0, safeValue = 0, blackoutValue = 0;
};
struct FixtureProfile {
    std::string id, name;
    int version = 1;
    std::vector<FixtureChannel> channels;       // footprint = sum of byteWidth
};
struct FixturePatch {
    std::uint32_t id = 0;
    std::string profileId, label;
    std::uint16_t universe = 1;                 // wire address (Art-Net 0-based / sACN 1-based: see lighting module)
    std::uint16_t startAddress = 1;             // 1..512
    std::uint32_t group = 0;
};

enum class LightingProtocol : std::uint8_t { ArtNet, Sacn };
struct LightingSettings {
    LightingProtocol protocol = LightingProtocol::ArtNet;
    std::string destination;                    // unicast IP, empty = none
    bool broadcast = false, multicast = false;
    std::uint16_t sacnPriority = 100;
    int rateHz = 25;                            // 25 default, 40 max
};

struct QualityProfileRef { std::string profileHash; bool locked = false; };

struct JournalEntry {
    Ns monoNs = 0; std::int64_t wallUnixMs = 0;
    std::string kind;                           // "scene","cue","mode","text","tempo","marker","device","adapt","rec","cast","gap"
    std::string detail;                         // sanitized
};

struct ShowDocument {
    std::uint32_t schemaMajor = kSchemaMajor, schemaMinor = kSchemaMinor;
    std::string uuid, name = "Untitled show";
    std::string appVersion;
    std::vector<SceneDef> scenes;
    std::vector<CueDef> cues;
    std::vector<OutputDef> outputs{OutputDef{}};
    std::vector<MediaRef> media;
    std::vector<TextMessage> messages;
    std::vector<MidiMapping> midi;
    std::vector<FixtureProfile> fixtureProfiles;
    std::vector<FixturePatch> patches;
    LightingSettings lighting;
    QualityProfileRef quality;
    ShowMode mode = ShowMode::Auto;
    std::uint64_t seed = 1;
    int minDwellSec = 8, repeatHistory = 3;
    bool strobingDisabled = false;
    std::vector<JournalEntry> journal;
};

}  // namespace ss::model
