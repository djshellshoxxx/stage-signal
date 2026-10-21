// StageSignal midi: rate ceiling and feedback protection (REQ-M03, spec section 14, RISK-09).
#pragma once
#include <cstdint>
#include <vector>
#include "ss/midi/limits.hpp"
#include "ss/midi/message.hpp"
#include "ss/model/show_model.hpp"

namespace ss::midi {

// Exact sliding-window ceiling: at most `ceiling` admissions in any window of `windowNs`.
class RateLimiter {
public:
    explicit RateLimiter(std::uint32_t ceiling = kRateCeilingPerSec, Ns windowNs = kRateWindowNs);
    bool tryAcquire(Ns now);
    std::uint64_t admitted() const { return admitted_; }
    std::uint64_t rejected() const { return rejected_; }
private:
    std::vector<Ns> ring_;
    std::size_t head_ = 0, count_ = 0;
    Ns window_;
    std::uint64_t admitted_ = 0, rejected_ = 0;
};

struct Signature {
    std::uint32_t port = 0;
    model::MidiMsgType type = model::MidiMsgType::Note;
    std::uint8_t channel = 0, number = 0;
    bool operator==(const Signature& o) const {
        return port == o.port && type == o.type && channel == o.channel && number == o.number;
    }
};
// Returns false for messages without a mappable signature (realtime, sysex).
bool signatureOf(std::uint32_t port, const Message& m, Signature& out);

struct FeedbackAlarm {
    Signature sig;
    std::uint32_t count = 0;            // events seen inside the window when it latched
    Ns at = 0;
    bool outputSuspended = false;       // an echoing output route was suspended too
};

class FeedbackGuard {
public:
    // ---- explicit enable of feedback-risk output routes ----
    void setRouteEnabled(std::uint32_t outPort, bool on);
    bool routeEnabled(std::uint32_t outPort) const;

    // ---- echo / recursion ----
    void recordOutput(std::uint32_t outPort, const Message& m, Ns now);
    // true when `m` (arriving on a linked input) matches an output we sent within kEchoWindowNs on one of `outPorts`
    bool isEcho(const std::vector<std::uint32_t>& outPorts, const Message& m, Ns now) const;

    // ---- storm latch ----
    // Count one identical trigger event. Returns true when this event latched a NEW alarm.
    bool noteTrigger(const Signature& sig, std::uint8_t value, Ns now);
    bool inputSuspended(const Signature& sig) const;
    bool outputSuspended(std::uint32_t outPort, const Message& m) const;
    const std::vector<FeedbackAlarm>& alarms() const { return alarms_; }
    bool clearAlarm(const Signature& sig);            // operator action: resume the route
    void reset();                                      // new show / all ports closed
private:
    struct OutRec { std::uint32_t outPort; model::MidiMsgType type; std::uint8_t ch, num; Ns at; };
    struct Track { Signature sig; std::uint8_t value; std::vector<Ns> times; Ns last; };
    struct OutSusp { std::uint32_t outPort; model::MidiMsgType type; std::uint8_t ch, num; Signature owner; };
    std::vector<std::uint32_t> enabled_;
    std::vector<OutRec> history_;
    std::size_t histHead_ = 0;
    std::vector<Track> tracks_;
    std::vector<FeedbackAlarm> alarms_;
    std::vector<OutSusp> outSusp_;
};

}  // namespace ss::midi
