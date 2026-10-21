#include "ss/midi/learn.hpp"
#include <algorithm>

namespace ss::midi {

static double norm(std::uint8_t v, std::uint8_t lo, std::uint8_t hi) {
    if (hi <= lo) return v >= lo ? 1.0 : 0.0;
    return std::min(1.0, std::max(0.0, (static_cast<double>(v) - lo) / (static_cast<double>(hi) - lo)));
}
double LearnCapture::normalisedCurrent() const { return norm(currentValue, rangeMin, rangeMax); }
double LearnCapture::normalisedObservedMin() const { return norm(observedMin, rangeMin, rangeMax); }
double LearnCapture::normalisedObservedMax() const { return norm(observedMax, rangeMin, rangeMax); }

void LearnSession::begin(const LearnFilter& f) { active_ = true; captured_ = false; filter_ = f; cap_ = LearnCapture{}; }
void LearnSession::cancel() { active_ = false; captured_ = false; cap_ = LearnCapture{}; }

bool LearnSession::offer(const PortInfo& port, const Message& m) {
    if (!active_) return false;
    if (!filter_.anyPort && port.portId != filter_.portId) return false;
    if (!m.isChannelMessage()) return false;
    if (filter_.channel != 255 && m.channel != filter_.channel) return false;

    model::MidiMsgType type;
    std::uint8_t value = 0;
    switch (m.kind) {
        case MsgKind::NoteOn: type = model::MidiMsgType::Note; value = m.data2; break;
        case MsgKind::NoteOff: type = model::MidiMsgType::Note; value = 0; break;
        case MsgKind::ControlChange: type = model::MidiMsgType::ControlChange; value = m.data2; break;
        case MsgKind::ProgramChange: type = model::MidiMsgType::ProgramChange; value = m.data1; break;
        default: return false;
    }

    if (!captured_) {
        if (m.kind == MsgKind::NoteOff) return false;       // a stray release must not bind
        captured_ = true;
        cap_ = LearnCapture{};
        cap_.portId = port.portId; cap_.deviceIdentity = port.identity; cap_.deviceName = port.name;
        cap_.channel = m.channel; cap_.type = type; cap_.number = m.data1;
        cap_.firstValue = cap_.currentValue = cap_.observedMin = cap_.observedMax = value;
        return true;
    }
    // Already captured: keep tracking only the same control; everything else is not ours.
    if (port.portId != cap_.portId || m.channel != cap_.channel || type != cap_.type || m.data1 != cap_.number)
        return false;
    cap_.currentValue = value;
    cap_.observedMin = std::min(cap_.observedMin, value);
    cap_.observedMax = std::max(cap_.observedMax, value);
    return true;
}

Status LearnSession::setRange(std::uint8_t lo, std::uint8_t hi) {
    if (!captured_) return makeError(ErrorCategory::InvalidArgument, "nothing learned yet");
    if (lo > hi || hi > 127) return makeError(ErrorCategory::InvalidArgument, "invalid range");
    cap_.rangeMin = lo; cap_.rangeMax = hi;
    return okStatus();
}

void LearnSession::useObservedRange() {
    if (!captured_) return;
    cap_.rangeMin = cap_.observedMin; cap_.rangeMax = cap_.observedMax;
}

Result<model::MidiMapping> LearnSession::commit(ActionId action, model::EncoderMode mode) {
    if (!captured_) return makeError(ErrorCategory::InvalidArgument, "nothing learned yet");
    model::MidiMapping m;
    m.deviceIdentity = cap_.deviceIdentity;
    m.deviceName = cap_.deviceName;
    m.channel = cap_.channel;
    m.type = cap_.type;
    m.number = cap_.number;
    m.valueMin = cap_.rangeMin;
    m.valueMax = cap_.rangeMax;
    m.encoder = mode;
    m.actionId = id(action);
    if (auto s = MappingTable::validate(m); !s.ok()) return s.error();   // e.g. relative mode on a note
    active_ = false; captured_ = false;
    return m;
}

}  // namespace ss::midi
