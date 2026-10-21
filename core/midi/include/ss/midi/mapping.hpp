// StageSignal midi: mapping table over model::MidiMapping (REQ-M01), device matching, encoder decoding.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/actions.hpp"
#include "ss/base/error.hpp"
#include "ss/base/types.hpp"
#include "ss/midi/message.hpp"
#include "ss/model/show_model.hpp"

namespace ss::midi {

// A known MIDI port (input side). identity is the stable id the app adapter derives (e.g. USB vendor/product/serial
// or OS endpoint id); name is the human name used only as a confirmed fallback.
struct PortInfo {
    std::uint32_t portId = 0;
    std::string identity, name;
    Generation generation = 0;
    bool available = false;
    Ns lostAt = 0;                    // valid when !available and it was once open
    bool everOpened = false;
};

enum class ActionClass : std::uint8_t { Trigger, Continuous };
ActionClass classifyAction(ActionId a);       // continuous = look controls, SetBpm, effect amounts; everything else triggers
bool isSceneTrigger(ActionId a);              // 50 ms debounce applies

enum class DeviceMatch : std::uint8_t { None, Identity, NameConfirmed, NameUnconfirmed };

struct MappingOptions {
    double triggerValue = 0.0;        // value carried by trigger actions (SceneSelect slot, GoToCue index, SetMode, ...)
    double relativeStep = 1.0 / 127.0;// normalised change per encoder tick (relative modes)
    double initialValue = 0.0;        // starting point for relative accumulation until the show reports a value
};

struct MappingEntry {
    model::MidiMapping map;
    MappingOptions opt;
    bool nameFallbackConfirmed = false;   // operator confirmed name-only device match (spec section 14)
    // runtime state (owned by the engine thread)
    double value = 0.0;                   // accumulated normalised value for relative modes
    bool armed = true;                    // CC-trigger edge detector
    bool hasTrigger = false;
    Ns lastTrigger = 0;
};

// Decoding helpers (pure).
double normaliseAbsolute(const model::MidiMapping& m, std::uint8_t raw);    // clamp((raw-min)/(max-min), 0, 1)
int relativeDelta(model::EncoderMode mode, std::uint8_t raw);               // signed ticks; 0 for Absolute
bool matchesSignature(const model::MidiMapping& m, const Message& msg);     // channel/type/number (not device)
DeviceMatch matchDevice(const MappingEntry& e, const PortInfo& p);

class MappingTable {
public:
    static Status validate(const model::MidiMapping& m);
    // Adds a mapping. id 0 assigns the next free id. Fails on invalid mapping, duplicate id, or 512 mappings.
    Result<std::uint32_t> add(model::MidiMapping m, MappingOptions opt = {});
    Status update(std::uint32_t id, model::MidiMapping m);                  // keeps id; resets runtime state
    Status setOptions(std::uint32_t id, MappingOptions opt);
    Status remove(std::uint32_t id);
    Status setNameFallbackConfirmed(std::uint32_t id, bool confirmed);
    Status load(const std::vector<model::MidiMapping>& list);              // replace all (show load); all-or-nothing
    std::vector<model::MidiMapping> toModel() const;
    const MappingEntry* find(std::uint32_t id) const;
    MappingEntry* findMutable(std::uint32_t id);
    const std::vector<MappingEntry>& entries() const { return entries_; }
    std::vector<MappingEntry>& entriesMutable() { return entries_; }
    std::size_t size() const { return entries_.size(); }
    void clear() { entries_.clear(); }
private:
    std::uint32_t nextFreeId() const;
    std::vector<MappingEntry> entries_;
};

}  // namespace ss::midi
