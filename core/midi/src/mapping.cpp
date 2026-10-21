#include "ss/midi/mapping.hpp"
#include <algorithm>
#include <cmath>

namespace ss::midi {

ActionClass classifyAction(ActionId a) {
    switch (a) {
        case ActionId::Brightness: case ActionId::Speed: case ActionId::Intensity:
        case ActionId::ColorHue: case ActionId::ColorSaturation: case ActionId::SetBpm:
        case ActionId::EffectPrismMirror: case ActionId::EffectBloomPulse:
            return ActionClass::Continuous;
        default: return ActionClass::Trigger;
    }
}

bool isSceneTrigger(ActionId a) {
    switch (a) {
        case ActionId::SceneSelect: case ActionId::NextScene: case ActionId::PrevScene: case ActionId::BankNext:
        case ActionId::BankPrev: case ActionId::NextCue: case ActionId::PrevCue: case ActionId::GoToCue:
            return true;
        default: return false;
    }
}

double normaliseAbsolute(const model::MidiMapping& m, std::uint8_t raw) {
    if (m.valueMax <= m.valueMin) return raw >= m.valueMin ? 1.0 : 0.0;
    const double v = (static_cast<double>(raw) - m.valueMin) / (static_cast<double>(m.valueMax) - m.valueMin);
    return std::min(1.0, std::max(0.0, v));
}

int relativeDelta(model::EncoderMode mode, std::uint8_t raw) {
    raw &= 0x7F;
    switch (mode) {
        case model::EncoderMode::Absolute: return 0;
        case model::EncoderMode::RelativeTwosComplement: return raw < 64 ? raw : static_cast<int>(raw) - 128;
        case model::EncoderMode::RelativeBinaryOffset: return static_cast<int>(raw) - 64;
        case model::EncoderMode::RelativeSignBit: {
            const int mag = raw & 0x3F;
            return (raw & 0x40) ? -mag : mag;
        }
    }
    return 0;
}

bool matchesSignature(const model::MidiMapping& m, const Message& msg) {
    if (!msg.isChannelMessage()) return false;
    if (m.channel != 255 && m.channel != msg.channel) return false;
    switch (m.type) {
        case model::MidiMsgType::Note:
            return (msg.kind == MsgKind::NoteOn || msg.kind == MsgKind::NoteOff) && msg.data1 == m.number;
        case model::MidiMsgType::ControlChange: return msg.kind == MsgKind::ControlChange && msg.data1 == m.number;
        case model::MidiMsgType::ProgramChange: return msg.kind == MsgKind::ProgramChange && msg.data1 == m.number;
    }
    return false;
}

DeviceMatch matchDevice(const MappingEntry& e, const PortInfo& p) {
    if (!e.map.deviceIdentity.empty() && e.map.deviceIdentity == p.identity) return DeviceMatch::Identity;
    if (!e.map.deviceName.empty() && e.map.deviceName == p.name)
        return e.nameFallbackConfirmed ? DeviceMatch::NameConfirmed : DeviceMatch::NameUnconfirmed;
    return DeviceMatch::None;
}

Status MappingTable::validate(const model::MidiMapping& m) {
    auto bad = [](const char* why) { return Status(makeError(ErrorCategory::InvalidArgument, why)); };
    if (m.actionId == 0) return bad("mapping has no action");
    if (m.deviceIdentity.empty() && m.deviceName.empty()) return bad("mapping must name a device");
    if (m.channel > 15 && m.channel != 255) return bad("channel must be 0..15 or omni");
    if (m.number > 127) return bad("number must be 0..127");
    if (m.valueMin > 127 || m.valueMax > 127) return bad("value range must be 0..127");
    if (m.valueMin > m.valueMax) return bad("value range is inverted");
    if (m.encoder != model::EncoderMode::Absolute) {
        if (m.type != model::MidiMsgType::ControlChange) return bad("relative encoders require a control change");
        if (classifyAction(static_cast<ActionId>(m.actionId)) != ActionClass::Continuous)
            return bad("relative encoders require a continuous action");
    }
    return okStatus();
}

std::uint32_t MappingTable::nextFreeId() const {
    std::uint32_t mx = 0;
    for (const auto& e : entries_) mx = std::max(mx, e.map.id);
    return mx + 1;
}

Result<std::uint32_t> MappingTable::add(model::MidiMapping m, MappingOptions opt) {
    if (entries_.size() >= model::Limits::kMaxMidiMappings)
        return makeError(ErrorCategory::ResourceExhausted, "mapping limit reached");
    if (auto s = validate(m); !s.ok()) return s.error();
    if (m.id == 0) m.id = nextFreeId();
    else if (find(m.id)) return makeError(ErrorCategory::InvalidArgument, "duplicate mapping id");
    MappingEntry e;
    e.map = std::move(m);
    e.opt = opt;
    e.value = std::min(1.0, std::max(0.0, opt.initialValue));
    const std::uint32_t id = e.map.id;
    entries_.push_back(std::move(e));
    return id;
}

Status MappingTable::update(std::uint32_t id, model::MidiMapping m) {
    MappingEntry* e = findMutable(id);
    if (!e) return makeError(ErrorCategory::InvalidArgument, "unknown mapping id");
    if (auto s = validate(m); !s.ok()) return s;
    m.id = id;
    const bool sameDevice = m.deviceIdentity == e->map.deviceIdentity && m.deviceName == e->map.deviceName;
    e->map = std::move(m);
    if (!sameDevice) e->nameFallbackConfirmed = false;       // changing the device voids the confirmation
    e->value = std::min(1.0, std::max(0.0, e->opt.initialValue));
    e->armed = true; e->hasTrigger = false;
    return okStatus();
}

Status MappingTable::setOptions(std::uint32_t id, MappingOptions opt) {
    MappingEntry* e = findMutable(id);
    if (!e) return makeError(ErrorCategory::InvalidArgument, "unknown mapping id");
    if (!(opt.relativeStep > 0.0) || opt.relativeStep > 1.0 || !std::isfinite(opt.triggerValue))
        return makeError(ErrorCategory::InvalidArgument, "bad mapping option");
    e->opt = opt;
    e->value = std::min(1.0, std::max(0.0, opt.initialValue));
    return okStatus();
}

Status MappingTable::remove(std::uint32_t id) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const MappingEntry& e) { return e.map.id == id; });
    if (it == entries_.end()) return makeError(ErrorCategory::InvalidArgument, "unknown mapping id");
    entries_.erase(it);
    return okStatus();
}

Status MappingTable::setNameFallbackConfirmed(std::uint32_t id, bool confirmed) {
    MappingEntry* e = findMutable(id);
    if (!e) return makeError(ErrorCategory::InvalidArgument, "unknown mapping id");
    e->nameFallbackConfirmed = confirmed;
    return okStatus();
}

Status MappingTable::load(const std::vector<model::MidiMapping>& list) {
    if (list.size() > model::Limits::kMaxMidiMappings)
        return makeError(ErrorCategory::ResourceExhausted, "mapping limit exceeded");
    MappingTable tmp;
    for (const auto& m : list) {
        auto r = tmp.add(m);
        if (!r.ok()) return r.error();
    }
    entries_ = std::move(tmp.entries_);
    return okStatus();
}

std::vector<model::MidiMapping> MappingTable::toModel() const {
    std::vector<model::MidiMapping> v;
    v.reserve(entries_.size());
    for (const auto& e : entries_) v.push_back(e.map);
    return v;
}

const MappingEntry* MappingTable::find(std::uint32_t id) const {
    for (const auto& e : entries_) if (e.map.id == id) return &e;
    return nullptr;
}
MappingEntry* MappingTable::findMutable(std::uint32_t id) {
    for (auto& e : entries_) if (e.map.id == id) return &e;
    return nullptr;
}

}  // namespace ss::midi
