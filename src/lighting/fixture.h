// Fixture profiles (data, never scripts), patch, validation, JSON (spec §14, REQ-L01).
// Raw channel values are in the channel's own width: 0..255 (width 1) or 0..65535 (width 2).
// Pan/tilt and fine-channel (16-bit) semantics are NOT hardware-qualified; built-in profiles are generic beta.
#pragma once
#include "core/types.h"
#include <map>
#include <string>
#include <vector>

namespace ss {

enum class ChannelFunction : uint8_t { Intensity, Color, Strobe, Pan, Tilt, Macro, Generic };
enum class ColorComponent : uint8_t { None, Red, Green, Blue, White, Amber, Uv };
enum class ByteOrder : uint8_t { CoarseFirst, FineFirst };   // slot order of a 16-bit channel

// A sub-range of a channel with one meaning. continuous=false: a macro/function range (never interpolated through).
struct FunctionRange { uint32_t lo = 0, hi = 0; bool continuous = true; std::string label; };

struct ChannelDef {
  std::string name;
  ChannelFunction function = ChannelFunction::Generic;
  ColorComponent color = ColorComponent::None;
  int offset = 0;                 // 0-based slot offset from start address (first slot of this channel)
  int width = 1;                  // 1 or 2 slots
  ByteOrder order = ByteOrder::CoarseFirst;
  uint32_t legalMin = 0, legalMax = 255;
  uint32_t defaultValue = 0, safeValue = 0;
  bool hasBlackout = false; uint32_t blackoutValue = 0;
  bool intensity = false, strobe = false;           // explicit designation (also implied by function)
  std::vector<FunctionRange> ranges;                // empty = whole legal range is one continuous function
  uint32_t maxRaw() const { return width == 2 ? 65535u : 255u; }
  bool isIntensity() const { return intensity || function == ChannelFunction::Intensity; }
  bool isStrobe() const { return strobe || function == ChannelFunction::Strobe; }
  uint32_t blackout() const { return hasBlackout ? blackoutValue : safeValue; }   // NOT blindly zero
  int rangeIndex(uint32_t v) const;                 // index into ranges (or 0 if none defined); -1 if in a gap
};

struct PanTiltLimits { bool present = false; double panMinDeg = 0, panMaxDeg = 0, tiltMinDeg = 0, tiltMaxDeg = 0; };

struct FixtureProfile {
  std::string id, name, manufacturer, notes;
  int version = 1;
  int channelCount = 0;           // DMX footprint (slots)
  bool qualified = false;         // hardware-qualified? (built-ins: false / beta)
  std::vector<ChannelDef> channels;
  PanTiltLimits panTilt;
  int footprint() const { return channelCount; }
  int findChannel(const std::string& name) const;
  Status validate() const;
};

// Writes raw value into the profile-relative slot buffer honoring width and coarse/fine order.
void encodeChannel(const ChannelDef&, uint32_t raw, uint8_t* slotsAtStart);
uint32_t decodeChannel(const ChannelDef&, const uint8_t* slotsAtStart);
// Fade one channel in the PHYSICAL domain (raw DMX values). Never interpolates between different function ranges
// or through a non-continuous (macro) range: in that case the 'from' value is held until the fade completes.
uint32_t fadeValue(const ChannelDef&, uint32_t from, uint32_t to, Ns elapsed, Ns duration);

std::string profileToJson(const FixtureProfile&);
Status profileFromJson(const std::string& text, FixtureProfile& out);   // validates; never throws

class ProfileLibrary {
 public:
  static const ProfileLibrary& builtin();   // generic-dimmer, generic-rgb, generic-rgbw, generic-rgb-strobe, generic-rgbw-strobe
  Status add(const FixtureProfile&);        // validates, rejects duplicates
  Status addJson(const std::string& text);
  const FixtureProfile* find(const std::string& id) const;
  std::vector<std::string> ids() const;
 private:
  std::map<std::string, FixtureProfile> m_;
};

enum class Ownership : uint8_t { Unresolved, Owner, Yielded };   // conflict resolution: one Owner + the rest Yielded
struct PatchEntry {
  std::string fixtureId, profileId, label;
  int universe = 0;               // protocol-native number (Art-Net: wire port-address 0..32767; sACN 1..63999)
  int startAddress = 1;           // 1..512
  Ownership ownership = Ownership::Unresolved;
};
struct PatchConflict { size_t a = 0, b = 0; int universe = 0, firstSlot = 0, lastSlot = 0; bool resolved = false; };
struct PatchReport {
  std::vector<std::string> errors, warnings;   // errors: patch invalid. warnings: conflicts etc
  std::vector<PatchConflict> conflicts;
  bool ok() const { return errors.empty(); }
  size_t unresolved() const { size_t n = 0; for (auto& c : conflicts) if (!c.resolved) ++n; return n; }
  bool canArm() const { return ok() && unresolved() == 0; }
};

struct Patch {
  std::vector<PatchEntry> entries;
  // minUniverse/maxUniverse: the protocol's legal universe range (Art-Net 0..32767, sACN 1..63999).
  PatchReport validate(const ProfileLibrary&, int minUniverse, int maxUniverse) const;
  std::string toJson() const;
  static Status fromJson(const std::string& text, Patch& out);
};

} // namespace ss
