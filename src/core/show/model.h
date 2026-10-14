// ShowDocument model (spec §17, IF-11). Plain data, no pointers; validated as a whole by validateShow().
// Owner: MOD-04. Persistence (MOD-13) converts this to/from JSON (persistence/show_json.h).
#pragma once
#include "core/types.h"
#include <map>
#include <string>
#include <vector>

namespace ss {
namespace show {

constexpr int kSchemaMajor = 1;   // reader rejects newer major (SchemaUnsupported)
constexpr int kSchemaMinor = 0;

// Import limits (spec §17/§13) - checked before allocation by ShowIO / bundle reader.
namespace limits {
constexpr size_t   kMaxJsonBytes      = 8u * 1024u * 1024u;
constexpr size_t   kMaxCues           = 10000;
constexpr size_t   kMaxScenes         = 1000;
constexpr size_t   kMaxMessages       = 256;
constexpr size_t   kMaxMidiMappings   = 512;
constexpr size_t   kMaxFixtures       = 256;   // applies to FixtureProfiles and Patch each
constexpr size_t   kMaxArchiveEntries = 10000;
constexpr uint64_t kMaxDecompressed   = 2ull * 1024 * 1024 * 1024;
constexpr size_t   kMaxTextCodePoints = 4096;
constexpr size_t   kMaxOutputs        = 16;
constexpr size_t   kMaxMedia          = 10000;
constexpr size_t   kMaxPool           = 1000;
constexpr size_t   kMaxOpaqueBytes    = 64 * 1024;   // per opaque JSON item
constexpr size_t   kMaxJournal        = 10000;
constexpr size_t   kMaxParams         = 32;
}

// ---- Renderer pack registry (spec §13: 12 original procedural scenes) -------------------------
struct ParamDef { const char* name; float min, max, def; bool strobe; /*value>0 means flashing*/ };
struct RendererInfo { const char* id; bool strobeCapable; std::vector<ParamDef> params; };
const std::vector<RendererInfo>& rendererPack();          // exactly 12 entries
const RendererInfo* findRenderer(const std::string& id);  // nullptr if unknown
const ParamDef* findParam(const RendererInfo& r, const std::string& name);

// ---- Records ----------------------------------------------------------------------------------
struct Scene {
  std::string id, name, rendererType;
  std::map<std::string, float> params;     // keys must be declared by the renderer; values in declared range
  bool strobe = false;                     // must be true if any strobe param > 0
  int cost = 1;                            // estimated relative cost 1..10
};
struct Transition { std::string type = "cut"; int ms = 0; };   // type: cut|fade|dip_black|wipe ; ms 0..60000 (elapsed time)
enum class Quantize : uint8_t { Immediate, Beat, Bar };
struct CueDuration {
  bool beats = false;       // false: elapsed ms (integer units); true: musical beats
  int64_t ms = 0;           // elapsed duration, 0 = hold until operator advances (free time)
  double beatCount = 0;     // musical beats (0 = hold)
};
struct Cue {
  std::string id;
  std::string sceneId;      // empty = keep current scene
  CueDuration duration;
  Transition transition;
  Quantize quantize = Quantize::Immediate;   // when this cue auto-starts relative to the beat grid
  std::string mediaRef;     // media hash, optional
  std::string textRef;      // text message id, optional (never auto-publishes, REQ-S05)
  std::string lightingRef;  // opaque lighting cue/look id, optional
};
enum class OutputRole : uint8_t { Audience, Program, Controls };
struct OverlayPolicy {      // REQ-A09: per logical visual output
  bool text = true, tempo = false, key = false, branding = false;
  std::string brandingRef;  // media hash of approved branding
};
struct Output {
  std::string logicalId;    // never a display index or physical id
  OutputRole role = OutputRole::Audience;
  OverlayPolicy overlay;
  std::string displayHint;  // optional non-secret matching hint (export-safe)
};
struct MediaRecord { std::string hash /*sha256 hex*/, relPath, format; uint64_t bytes = 0; };
struct TextMessage { std::string id, text /*UTF-8*/; };
struct PoolEntry { std::string sceneId; int weight = 1; /*0..1000, integer for portable determinism*/ };
struct AutoConfig {
  uint64_t seed = 1;
  std::vector<PoolEntry> pool;        // empty = all scenes weight 1
  int minDwellMs = 8000, maxDwellMs = 16000;   // 500..3600000
  int repeatHistory = 2;              // 0..64 most recent scenes (incl. on-air) not re-selectable
  Quantize quantize = Quantize::Beat; // changes land on beat/bar when tempo valid
  int beatsPerBar = 4;                // 1..16; bar phase is an assumption from the beat anchor
  Transition transition{"fade", 500};
};
enum class AudioLossPolicy : uint8_t { Continue, Idle };
struct IdleConfig {
  AudioLossPolicy audioLoss = AudioLossPolicy::Continue;
  std::string screen = "black";       // black|logo|color_field|scene
  std::string sceneId;                // when screen == scene
  int audioLossMs = 1000;             // 250..1500 (+ one tick) keeps REQ-D09 within 2 s
};
enum class CueListEnd : uint8_t { Hold, Loop };
struct JournalEntry { int64_t wallMs = 0; std::string kind, json; };   // pointer-free

struct ShowDocument {
  std::string uuid;                   // canonical 8-4-4-4-12 hex
  std::string name;
  int schemaMajor = kSchemaMajor, schemaMinor = kSchemaMinor;
  std::vector<std::string> migrationHistory;   // e.g. "0.0->1.0"
  std::vector<Scene> scenes;
  std::vector<Cue> cues;
  CueListEnd cueEnd = CueListEnd::Hold;
  std::vector<Output> outputs;
  std::vector<MediaRecord> media;
  std::vector<TextMessage> texts;
  std::vector<std::string> midiMappings;       // opaque JSON object text per mapping (MOD-09 owns schema)
  std::vector<std::string> fixtureProfiles;    // opaque JSON
  std::vector<std::string> patch;              // opaque JSON
  std::string qualityProfile;                  // reference id only
  AutoConfig autoCfg;
  IdleConfig idle;
  int defaultMode = 0;                         // 0 Auto, 1 Programmed, 2 Hybrid
  double manualBpm = 0;                        // 0 = none; persisted override (REQ-A06)
  int manualKey = -1;                          // -1 none; pc + 12*minor (REQ-A08)
  bool disableStrobing = false;
  std::vector<JournalEntry> journal;
  std::map<std::string, std::string> meta;     // preserved "x_*" metadata: key -> JSON text
};

// UTF-8 strictness: rejects overlong, surrogates, > U+10FFFF, truncated. Counts code points.
bool utf8Valid(const std::string& s, size_t* codePoints = nullptr);
bool validId(const std::string& s);                       // [A-Za-z0-9_.-]{1,64}
bool validUuid(const std::string& s);
bool safeRelPath(const std::string& p);                   // relative, no .., no drive/UNC/backslash
bool isHex(const std::string& s, size_t minLen, size_t maxLen);
// Full-document validation (limits, references, ranges). First failure -> InvalidArgument "where: why".
Status validateShow(const ShowDocument& d);
// Resolved params: declared defaults overlaid by the scene's values.
std::map<std::string, float> resolvedParams(const Scene& s);
bool sceneIsStrobe(const Scene& s);
const char* const* mediaFormats(size_t* n);

} // namespace show
} // namespace ss
