// MOD-09 MIDI mapping model, table, learn mode, JSON (REQ-M01, spec §14). Portable C++17.
#pragma once
#include "core/types.h"
#include <mutex>
#include <map>
#include <string>
#include <vector>

namespace ss {

enum class MidiMsgType : uint8_t { Note, CC, ProgramChange };
// Encoder modes are always explicit; a CC mapping is inactive until the operator confirms the mode.
enum class MidiMode : uint8_t { Absolute, RelativeTwosComp, RelativeBinaryOffset, RelativeSignMagnitude };

const char* midiTypeName(MidiMsgType t);
const char* midiModeName(MidiMode m);
bool midiTypeFromName(const std::string& s, MidiMsgType& out);
bool midiModeFromName(const std::string& s, MidiMode& out);
const char* midiActionName(ActionId id);                       // stable names used in JSON ("NextScene")
bool midiActionFromName(const std::string& s, ActionId& out);
bool midiActionIsContinuous(ActionId id);   // Brightness/Speed/Intensity/ColorHue/SetBpm: coalesce to latest
bool midiActionIsCritical(ActionId id);     // stops/blackouts/disarm: never coalesced/debounced, priority latch
bool midiActionIsSceneTrigger(ActionId id); // Next/Prev/Select scene+cue: 50 ms debounce
bool midiActionCarriesValue(ActionId id);   // fires even when the raw value is 0

// One decoded channel message. Note-on with velocity 0 is decoded as on=false (note-off).
struct MidiMsg {
  MidiMsgType type = MidiMsgType::Note;
  uint8_t channel = 0;   // 0..15
  uint8_t number = 0;    // note / controller / program
  uint8_t value = 0;     // velocity / cc value (0 for PC)
  bool on = true;        // Note only
  Ns arrivalNs = 0;
};
enum class MidiParse : uint8_t { Message, Realtime, Skipped, Invalid };
// Parses one message at d[0..n). `consumed` is always >=1 when n>0. Realtime (0xF8+) and SysEx/other are not Message.
MidiParse parseMidiMessage(const uint8_t* d, size_t n, size_t& consumed, MidiMsg& out);

struct MidiMapping {
  std::string id;                       // unique within a table; assigned on add() when empty
  std::string deviceId;                 // stable device id; empty => name fallback
  std::string deviceName;               // used when deviceId empty (needs deviceConfirmed)
  bool deviceConfirmed = false;         // operator confirmed name-fallback device
  int channel = -1;                     // 0..15, -1 = omni
  MidiMsgType type = MidiMsgType::CC;
  int number = 0;                       // 0..127; ProgramChange: -1 = any program (value = program)
  int valueMin = 0, valueMax = 127;     // accepted raw range (Absolute) 
  MidiMode mode = MidiMode::Absolute;
  bool modeConfirmed = false;           // CC mappings are inactive until true (no silent guessing)
  ActionId action = ActionId::None;
  int target = -1;
  double scaleMin = 0, scaleMax = 1;    // output value = scaleMin + norm*(scaleMax-scaleMin)
  double relStep = 1.0 / 127.0;         // relative: normalized change per encoder tick
  bool nameFallback() const { return deviceId.empty(); }
};
enum class MappingStatus : uint8_t { Active, DeviceUnavailable, NeedsDeviceConfirm, NeedsModeConfirm };

struct MidiMatch { std::string mappingId; Action action; bool critical = false, continuous = false, sceneTrigger = false; };
struct MsgClass { bool mapped = false, critical = false, continuous = false, relative = false; };  // relative: deltas, never replace-coalesce

struct LearnRequest {
  std::string deviceId, deviceName;     // chosen device (one of them non-empty)
  int channel = -1;                     // chosen channel, -1 = any (captured channel is bound)
  ActionId action = ActionId::None;
  int target = -1;
  double scaleMin = 0, scaleMax = 1;
  MidiMode mode = MidiMode::Absolute;
  bool modeExplicit = false;            // true only if the operator picked the mode
};
struct LearnInfo {
  bool active = false, captured = false;
  MidiMsgType type = MidiMsgType::CC;
  int channel = 0, number = 0;
  int rawValue = 0, minSeen = 0, maxSeen = 0;
  double normalized = 0;                // current value 0..1 over the 0..127 range
  double rangeLo = 0, rangeHi = 1;      // normalized range observed so far
};

// Thread-safe mapping table. Relative-encoder state lives here (current value per mapping).
class MappingTable {
 public:
  // inspect / edit / delete
  std::string add(MidiMapping m, std::string* whyNot = nullptr);   // "" on invalid
  bool edit(const std::string& id, MidiMapping m, std::string* whyNot = nullptr);
  bool remove(const std::string& id);
  bool get(const std::string& id, MidiMapping& out) const;
  std::vector<MidiMapping> list() const;
  size_t size() const;
  void clear();
  static bool validate(const MidiMapping& m, std::string* why);
  MappingStatus status(const std::string& id) const;               // Active when usable
  std::vector<std::string> unavailableMappings() const;            // ids whose device is lost
  void setCurrentValue(const std::string& id, double v);           // sync relative state to engine
  double currentValue(const std::string& id) const;

  // device availability (REQ-M04). Keys: port stableId and "name:"+portName are both marked.
  void setPortAvailable(const std::string& stableId, const std::string& name, bool available);
  bool portAvailable(const std::string& stableId, const std::string& name) const;

  // learn (REQ-M01)
  Status beginLearn(const LearnRequest& r);
  void cancelLearn();
  bool learning() const;
  // Consumes the message if it came from the learn device/channel (even if unsupported). Returns consumed.
  bool feedLearn(const std::string& devId, const std::string& devName, const MidiMsg& m);
  LearnInfo learnInfo() const;
  std::string commitLearn(bool useObservedRange = false, std::string* whyNot = nullptr);

  // runtime
  MsgClass classify(const std::string& devId, const std::string& devName, const MidiMsg& m) const;
  size_t translate(const std::string& devId, const std::string& devName, const MidiMsg& m, std::vector<MidiMatch>& out);

  // JSON schema "stagesignal.midi.mappings" v1; fromJson is atomic (table unchanged on error).
  std::string toJson() const;
  Status fromJson(const std::string& text);

 private:
  struct Entry { MidiMapping m; double cur = 0.5; };
  struct Learn { bool active = false, captured = false; LearnRequest req; MidiMapping cand; int raw = 0, lo = 0, hi = 0; };
  struct Dev { bool available = true; };
  bool usable(const Entry& e) const;
  bool devMatch(const MidiMapping& m, const std::string& devId, const std::string& devName) const;
  bool msgMatch(const Entry& e, const std::string& devId, const std::string& devName, const MidiMsg& m) const;
  MappingStatus statusLocked(const Entry& e) const;
  std::string freshId();
  mutable std::mutex m_;
  std::vector<Entry> v_;
  std::map<std::string, Dev> dev_;
  Learn learn_;
  uint64_t idCounter_ = 0;
};

} // namespace ss
