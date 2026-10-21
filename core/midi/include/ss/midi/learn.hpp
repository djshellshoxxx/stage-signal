// StageSignal midi: MIDI learn (REQ-M01, spec section 14).
// Consumes the next supported Note-on / CC / Program Change for the chosen device/channel, then keeps tracking
// that control so the UI can show the observed (normalised) range and current value until commit or cancel.
#pragma once
#include <cstdint>
#include <string>
#include "ss/base/error.hpp"
#include "ss/midi/mapping.hpp"
#include "ss/midi/message.hpp"

namespace ss::midi {

struct LearnFilter {
    bool anyPort = true;
    std::uint32_t portId = 0;          // when !anyPort
    std::uint8_t channel = 255;        // 255 = any channel
};

struct LearnCapture {
    std::uint32_t portId = 0;
    std::string deviceIdentity, deviceName;
    std::uint8_t channel = 0;
    model::MidiMsgType type = model::MidiMsgType::Note;
    std::uint8_t number = 0;
    std::uint8_t firstValue = 0, currentValue = 0, observedMin = 0, observedMax = 0;
    std::uint8_t rangeMin = 0, rangeMax = 127;           // range the mapping will use (default full 0..127)
    double normalisedCurrent() const;                    // current value within [rangeMin, rangeMax], 0..1
    double normalisedObservedMin() const;
    double normalisedObservedMax() const;
};

class LearnSession {
public:
    void begin(const LearnFilter& f = {});
    void cancel();
    bool active() const { return active_; }
    bool captured() const { return captured_; }
    const LearnCapture* capture() const { return captured_ ? &cap_ : nullptr; }
    // Returns true when the message was consumed (it must NOT also run mapped actions).
    bool offer(const PortInfo& port, const Message& m);
    Status setRange(std::uint8_t lo, std::uint8_t hi);
    void useObservedRange();
    // Builds the mapping (id 0 = table assigns) and ends the session. Fails when nothing was captured.
    Result<model::MidiMapping> commit(ActionId action, model::EncoderMode mode = model::EncoderMode::Absolute);
private:
    bool active_ = false, captured_ = false;
    LearnFilter filter_;
    LearnCapture cap_;
};

}  // namespace ss::midi
